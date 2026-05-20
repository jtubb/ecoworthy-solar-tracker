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

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(TrackerBridge)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
