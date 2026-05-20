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
LIBRARIES = ["rweather/Cryptography@^0.4.0"]  # AES-CCM; verify name on first compile

tracker_bridge_ns = cg.esphome_ns.namespace("tracker_bridge")
TrackerBridge = tracker_bridge_ns.class_(
    "TrackerBridge", cg.Component, uart.UARTDevice
)

CONF_TRACKER_BRIDGE_ID = "tracker_bridge_id"

# --- Phase 4: mesh config keys ---
CONF_MESH = "mesh"
CONF_CHANNEL = "channel"
CONF_PSK = "psk"
CONF_TRACKER_ID = "tracker_id"

CONFIG_SCHEMA = (
    cv.Schema({
        cv.GenerateID(): cv.declare_id(TrackerBridge),
        cv.Optional(CONF_MESH): cv.Schema({
            cv.Required(CONF_CHANNEL): cv.int_range(min=1, max=13),
            cv.Required(CONF_PSK): cv.string,   # 16-byte hex or passphrase
            cv.Required(CONF_TRACKER_ID): cv.string_strict,
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
        mesh = config[CONF_MESH]
        cg.add(var.set_mesh_channel(mesh[CONF_CHANNEL]))
        cg.add(var.set_mesh_psk(mesh[CONF_PSK]))
        cg.add(var.set_tracker_id(mesh[CONF_TRACKER_ID]))
