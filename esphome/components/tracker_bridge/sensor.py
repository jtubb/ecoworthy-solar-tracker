import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_WIND_SPEED,
    STATE_CLASS_MEASUREMENT,
    UNIT_METER_PER_SECOND,
    UNIT_PERCENT,
)
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_AZ = "az"
CONF_EL = "el"
CONF_WIND = "wind"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
        cv.Optional(CONF_AZ): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_EL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_WIND): sensor.sensor_schema(
            unit_of_measurement=UNIT_METER_PER_SECOND,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_WIND_SPEED,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    if CONF_AZ in config:
        s = await sensor.new_sensor(config[CONF_AZ])
        cg.add(parent.set_az_sensor(s))
    if CONF_EL in config:
        s = await sensor.new_sensor(config[CONF_EL])
        cg.add(parent.set_el_sensor(s))
    if CONF_WIND in config:
        s = await sensor.new_sensor(config[CONF_WIND])
        cg.add(parent.set_wind_sensor(s))
