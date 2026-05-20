import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_MODE = "mode"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
        cv.Optional(CONF_MODE): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    if CONF_MODE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_MODE])
        cg.add(parent.set_mode_sensor(ts))
