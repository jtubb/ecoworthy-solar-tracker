"""Optional `number` platform for tracker_bridge.

Exposes two kinds of number entities:

    wind_override:
        Writes directly to the bridge's local_wind_used_ cache, which
        is what mesh_tx_wind_/mesh_tx_telemetry_ broadcast on the mesh.
        Intended for bench-testing nodes with no STC attached -- lets
        you inject synthetic wind values from HA's UI and prove the
        remote-wind path end-to-end with a single real STC on the bench.
        Do NOT configure this on a node that DOES have an STC -- the
        STC poll (~2 s cadence) will overwrite whatever HA sets.

    wind_storm_mps / wind_release_mps / storm_dwell_min / track_thresh:
        RW config sliders (Phase 4 Task 11).  Moving a slider fires a
        CONFIG SET_REQ to the target tracker (local STC via UART, or a
        remote peer via the mesh).  Sliders are optimistic: displayed
        value reflects what was set, not an echo from the STC.
        Future Task 12 can add GET_RESP → publish_state read-back.

        Use peer_id: "T2" (matching a declared peer's id in mesh.peers)
        to control a remote tracker.  Omit peer_id (or set to "") to
        control the local STC.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_MIN_VALUE,
    CONF_MAX_VALUE,
    CONF_STEP,
    UNIT_METER_PER_SECOND,
)
from . import TrackerBridge, tracker_bridge_ns, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

# --- Existing bench-helper entity ---
CONF_WIND_OVERRIDE = "wind_override"

WindOverrideNumber = tracker_bridge_ns.class_(
    "WindOverrideNumber", number.Number, cg.Component
)

# --- Phase 4 Task 11: RW config sliders ---
CONF_PEER_ID = "peer_id"
CONF_WIND_STORM = "wind_storm_mps"
CONF_WIND_RELEASE = "wind_release_mps"
CONF_STORM_DWELL = "storm_dwell_min"
CONF_TRACK_THRESH = "track_thresh"

# --- Night-park config (firmware cfg field IDs 0x05-0x09) ---
# Local-only for v1: peer_id is silently ignored if non-empty.  Enable is a
# number 0/1 rather than a switch to keep platform surface small.
CONF_NIGHT_PARK_EN  = "night_park_enable"
CONF_NIGHT_PARK_AZ  = "night_park_az_pct"
CONF_NIGHT_PARK_EL  = "night_park_el_pct"
CONF_NIGHT_PARK_MIN = "night_park_dark_min"
CONF_NIGHT_PARK_THR = "night_park_dark_thr"

ConfigNumber = tracker_bridge_ns.class_("ConfigNumber", number.Number)

# Reusable schema for a single ConfigNumber entity.  Caller supplies
# meaningful min/max in YAML; step defaults to 1.
_CONFIG_NUM_SCHEMA = number.number_schema(ConfigNumber).extend(
    {
        cv.Optional(CONF_MIN_VALUE, default=0): cv.float_,
        cv.Optional(CONF_MAX_VALUE, default=100): cv.float_,
        cv.Optional(CONF_STEP, default=1): cv.float_,
    }
)

# Field IDs matching STC firmware's cfg_field_id_t enum in main.c.
# Keep in sync with the STC side (EcoWorthyFirmware/src/main.c).
_FIELD_ID = {
    CONF_WIND_STORM:     1,
    CONF_WIND_RELEASE:   2,
    CONF_STORM_DWELL:    3,
    CONF_TRACK_THRESH:   4,
    CONF_NIGHT_PARK_EN:  5,
    CONF_NIGHT_PARK_AZ:  6,
    CONF_NIGHT_PARK_EL:  7,
    CONF_NIGHT_PARK_MIN: 8,
    CONF_NIGHT_PARK_THR: 9,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
        # --- Bench-helper (existing) ---
        cv.Optional(CONF_WIND_OVERRIDE): number.number_schema(
            WindOverrideNumber,
            unit_of_measurement=UNIT_METER_PER_SECOND,
        ),
        # --- RW config sliders (Task 11) ---
        # Optional peer_id: empty / absent → local STC; set to a declared
        # peer id (e.g. "T2") to target a remote tracker via mesh CONFIG SET.
        cv.Optional(CONF_PEER_ID, default=""): cv.string_strict,
        cv.Optional(CONF_WIND_STORM):     _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_WIND_RELEASE):   _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_STORM_DWELL):    _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_TRACK_THRESH):   _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_NIGHT_PARK_EN):  _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_NIGHT_PARK_AZ):  _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_NIGHT_PARK_EL):  _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_NIGHT_PARK_MIN): _CONFIG_NUM_SCHEMA,
        cv.Optional(CONF_NIGHT_PARK_THR): _CONFIG_NUM_SCHEMA,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])

    # --- Bench-helper wind override ---
    if CONF_WIND_OVERRIDE in config:
        n = await number.new_number(
            config[CONF_WIND_OVERRIDE],
            min_value=0,
            max_value=99,
            step=1,
        )
        await cg.register_component(n, config[CONF_WIND_OVERRIDE])
        cg.add(n.set_parent(parent))

    # --- RW config sliders ---
    peer_id = config.get(CONF_PEER_ID, "")
    # Map config key → TrackerBridge setter name for the GET_RESP read-back wiring
    # (P5-2). After creating each ConfigNumber, call the matching setter so the
    # dispatcher's publish_state path has a pointer to reach without going through
    # the ConfigNumber class itself.
    _FIELD_SETTER = {
        CONF_WIND_STORM:     "set_wind_storm_number_for",
        CONF_WIND_RELEASE:   "set_wind_release_number_for",
        CONF_STORM_DWELL:    "set_storm_dwell_number_for",
        CONF_TRACK_THRESH:   "set_track_thresh_number_for",
        CONF_NIGHT_PARK_EN:  "set_night_park_en_number_for",
        CONF_NIGHT_PARK_AZ:  "set_night_park_az_number_for",
        CONF_NIGHT_PARK_EL:  "set_night_park_el_number_for",
        CONF_NIGHT_PARK_MIN: "set_night_park_min_number_for",
        CONF_NIGHT_PARK_THR: "set_night_park_thr_number_for",
    }
    for conf_key, field_id in _FIELD_ID.items():
        if conf_key not in config:
            continue
        num_cfg = config[conf_key]
        n = await number.new_number(
            num_cfg,
            min_value=num_cfg[CONF_MIN_VALUE],
            max_value=num_cfg[CONF_MAX_VALUE],
            step=num_cfg[CONF_STEP],
        )
        cg.add(n.set_parent(parent))
        cg.add(n.set_peer_id(peer_id))
        cg.add(n.set_field_id(field_id))
        # Wire the number pointer into TrackerBridge so GET_RESP dispatch can
        # call publish_state on it when the STC (local) or mesh (remote) delivers
        # an authoritative read-back value.
        setter = _FIELD_SETTER[conf_key]
        cg.add(getattr(parent, setter)(peer_id, n))
