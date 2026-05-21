"""ESPHome external component: solar tracker bridge.

Polls a custom-firmware STC15 dual-axis tracker board over half-duplex
single-wire 9600 8N1 and publishes az/el/wind/mode as Home Assistant
entities.  Wire framing is the same as the STC firmware:

    \\xAA\\x55  <ASCII payload>  <2-hex CRC8>  \\n

CRC-8/SMBUS (poly 0x07, init 0x00, MSB-first, no reflection, no final
XOR), computed over the payload bytes only.  See
`EcoWorthyFirmware/src/main.c` (uart_send_frame / uart_frame_feed /
crc8_update) for the matching reference implementation -- the two must
be byte-identical.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@jtubb"]
DEPENDENCIES = ["uart"]
MULTI_CONF = True

tracker_bridge_ns = cg.esphome_ns.namespace("tracker_bridge")
TrackerBridge = tracker_bridge_ns.class_(
    "TrackerBridge", cg.Component, uart.UARTDevice
)

CONF_TRACKER_BRIDGE_ID = "tracker_bridge_id"

# --- Phase 4: mesh config keys ---
CONF_MESH = "mesh"
CONF_CHANNEL = "channel"
CONF_PSK = "psk"
CONF_TEST_BROADCAST = "test_broadcast"
CONF_LOCAL_ROLE = "local_role"

# --- Phase 4 P4-12: peer list (name-only, no MAC) ---
CONF_PEERS = "peers"


def _validate_peer_name(value):
    """Validate peer name string; warn if > 31 chars (will be truncated at runtime)."""
    value = cv.string_strict(value)
    if len(value) > 31:
        raise cv.Invalid(
            f"Peer name '{value}' is {len(value)} chars; maximum is 31 "
            f"(will be truncated to fit the 32-byte node_name field)"
        )
    return value


CONFIG_SCHEMA = (
    cv.Schema({
        cv.GenerateID(): cv.declare_id(TrackerBridge),
        cv.Optional(CONF_MESH): cv.Schema({
            cv.Required(CONF_CHANNEL): cv.int_range(min=1, max=13),
            cv.Required(CONF_PSK): cv.string,   # 16-byte hex or passphrase
            # Bench validation: when true, emit a 2-byte test packet 3 s
            # after boot. Pair with a listener node; expect a log line
            # `rx type=99 from XX..XX plen=2`.  Leave unset in production.
            cv.Optional(CONF_TEST_BROADCAST, default=False): cv.boolean,
            # Role in the mesh: "primary" owns the wind sensor and broadcasts
            # WIND frames; "secondary" receives them and forwards to its STC.
            # Set per-device in YAML; defaults to "secondary" if unset.
            cv.Optional(CONF_LOCAL_ROLE, default="secondary"):
                cv.one_of("primary", "secondary", lower=True),
            # Declared peer trackers: list of esphome.name strings.
            # Binding key is the node_name (from App.get_name()), NOT MAC.
            # Each entry must match the 'name:' field in the peer's YAML.
            cv.Optional(CONF_PEERS, default=[]): cv.ensure_list(_validate_peer_name),
        }),
    })
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_MESH in config:
        # AES-128-GCM + SHA256 for the mesh.  rweather/Crypto exposes
        # <AES.h>, <GCM.h>, <SHA256.h>.  ESPHome wires this through to
        # PlatformIO's lib_deps for the build.
        cg.add_library("rweather/Crypto", "^0.4.0")
        mesh = config[CONF_MESH]
        cg.add(var.set_mesh_channel(mesh[CONF_CHANNEL]))
        cg.add(var.set_mesh_psk(mesh[CONF_PSK]))
        cg.add(var.set_test_broadcast(mesh[CONF_TEST_BROADCAST]))
        cg.add(var.set_local_role(1 if mesh[CONF_LOCAL_ROLE] == "primary" else 2))
        # Register declared peers before sensor/text_sensor/number platforms run
        # (this to_code executes first; platforms attach afterwards).
        peers = mesh.get(CONF_PEERS, [])
        # DEBUG: surface what schema gave us at compile time -- to_code can't
        # log to the device, but print() lands in `esphome compile` stdout.
        print(f"[tracker_bridge.__init__] CONF_MESH keys: {list(mesh.keys())}")
        print(f"[tracker_bridge.__init__] CONF_PEERS value: {peers!r}")
        # DEBUG: hardcoded call outside the loop -- if THIS doesn't show up in
        # the device log as "registered peer 'ZZZ-CODEGEN-TEST'" / setup()
        # peer_decls_ size>=1, then the entire to_code path is broken.
        cg.add(var.register_peer("ZZZ-CODEGEN-TEST"))
        for peer_name in peers:
            print(f"[tracker_bridge.__init__]   emitting register_peer({peer_name!r})")
            cg.add(var.register_peer(peer_name))
