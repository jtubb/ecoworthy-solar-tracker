import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_MODE = "mode"
CONF_PEER_ID = "peer_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
        # Optional peer_id: empty string / absent → local tracker (T1).
        # Set to a declared peer's id label (e.g. "T2") to attach this
        # text_sensor to that peer's mode slot in peer_decls_.
        cv.Optional(CONF_PEER_ID, default=""): cv.string_strict,
        cv.Optional(CONF_MODE): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    peer_id = config.get(CONF_PEER_ID, "")
    if CONF_MODE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_MODE])
        cg.add(parent.set_mode_sensor_for(peer_id, ts))
