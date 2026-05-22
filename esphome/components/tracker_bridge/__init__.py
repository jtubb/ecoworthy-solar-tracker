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
from esphome.core import CORE

CODEOWNERS = ["@jtubb"]
DEPENDENCIES = ["uart"]
# One tracker_bridge instance per device.  The C++ side uses a static
# singleton pointer for the ESP-NOW recv callback dispatch; a second
# instance would silently lose mesh frames for whichever one didn't
# grab the singleton last.  There's no physical scenario where two
# tracker_bridge blocks make sense on one ESP-01S (one STC, one HA
# endpoint per device), so disallow at codegen.
MULTI_CONF = False

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
# P6-1: has_wind_sensor replaces local_role.  Wind primary is auto-elected
# (lowest MAC among live has_wind_sensor peers wins).
CONF_HAS_WIND_SENSOR = "has_wind_sensor"

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
            # P6-1: capability bit advertised in TELEMETRY broadcasts.
            # Set true on nodes with a real wind sensor attached to the STC.
            # The wind primary is auto-elected: the live has_wind_sensor peer
            # with the lowest MAC broadcasts WIND frames; all others receive.
            # Defaults to false (no sensor, never elected primary).
            cv.Optional(CONF_HAS_WIND_SENSOR, default=False): cv.boolean,
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
        # The mesh binding key is the esphome.name (wire_id_), capped at 31
        # chars to fit the 32-byte NUL-terminated field.  Reject longer names
        # at codegen so the truncation case never reaches runtime.
        device_name = CORE.name
        if device_name and len(device_name) > 31:
            raise cv.Invalid(
                f"esphome.name '{device_name}' is {len(device_name)} chars; "
                f"maximum is 31 when mesh: is configured (the mesh wire format "
                f"reserves a 32-byte NUL-terminated node_name field).  "
                f"Shorten the name or omit the mesh: block."
            )
        # AES-128-GCM + SHA256 for the mesh.  rweather/Crypto exposes
        # <AES.h>, <GCM.h>, <SHA256.h>.  ESPHome wires this through to
        # PlatformIO's lib_deps for the build.
        cg.add_library("rweather/Crypto", "^0.4.0")
        mesh = config[CONF_MESH]
        cg.add(var.set_mesh_channel(mesh[CONF_CHANNEL]))
        cg.add(var.set_mesh_psk(mesh[CONF_PSK]))
        cg.add(var.set_test_broadcast(mesh[CONF_TEST_BROADCAST]))
        cg.add(var.set_has_wind_sensor(mesh[CONF_HAS_WIND_SENSOR]))
        # Register declared peers before sensor/text_sensor/number platforms run
        # (this to_code executes first; platforms attach afterwards).
        for peer_name in mesh.get(CONF_PEERS, []):
            cg.add(var.register_peer(peer_name))
