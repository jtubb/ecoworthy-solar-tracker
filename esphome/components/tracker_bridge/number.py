"""Optional `number` platform for tracker_bridge.

Currently exposes one entity:

    wind_override:
        Writes directly to the bridge's local_wind_used_ cache, which
        is what mesh_tx_wind_/mesh_tx_telemetry_ broadcast on the mesh.
        Intended for bench-testing nodes with no STC attached -- lets
        you inject synthetic wind values from HA's UI and prove the
        remote-wind path end-to-end with a single real STC on the bench.
        Do NOT configure this on a node that DOES have an STC -- the
        STC poll (~2 s cadence) will overwrite whatever HA sets.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import UNIT_METER_PER_SECOND
from . import TrackerBridge, tracker_bridge_ns, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_WIND_OVERRIDE = "wind_override"

WindOverrideNumber = tracker_bridge_ns.class_(
    "WindOverrideNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
        cv.Optional(CONF_WIND_OVERRIDE): number.number_schema(
            WindOverrideNumber,
            unit_of_measurement=UNIT_METER_PER_SECOND,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    if CONF_WIND_OVERRIDE in config:
        n = await number.new_number(
            config[CONF_WIND_OVERRIDE],
            min_value=0,
            max_value=99,
            step=1,
        )
        await cg.register_component(n, config[CONF_WIND_OVERRIDE])
        cg.add(n.set_parent(parent))
