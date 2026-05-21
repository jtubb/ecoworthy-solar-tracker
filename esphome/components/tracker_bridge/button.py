"""ESPHome platform: tracker_bridge buttons.

Exposes force_park, force_release, and stop as HA button entities.
Each press broadcasts the corresponding MSG_COMMAND to all mesh peers,
causing every tracker to execute the command on its local STC.

Usage in YAML:

  button:
    - platform: tracker_bridge
      tracker_bridge_id: bridge
      force_park:    { name: "Solar Tracker 1 Force Park" }
      force_release: { name: "Solar Tracker 1 Force Release" }
      stop:          { name: "Solar Tracker 1 Emergency Stop" }
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_FORCE_PARK    = "force_park"
CONF_FORCE_RELEASE = "force_release"
CONF_STOP          = "stop"

tracker_bridge_ns = cg.esphome_ns.namespace("tracker_bridge")
ForceParkButton    = tracker_bridge_ns.class_("ForceParkButton",    button.Button)
ForceReleaseButton = tracker_bridge_ns.class_("ForceReleaseButton", button.Button)
StopButton         = tracker_bridge_ns.class_("StopButton",         button.Button)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
    cv.Optional(CONF_FORCE_PARK):    button.button_schema(ForceParkButton),
    cv.Optional(CONF_FORCE_RELEASE): button.button_schema(ForceReleaseButton),
    cv.Optional(CONF_STOP):          button.button_schema(StopButton),
})


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    if CONF_FORCE_PARK in config:
        b = await button.new_button(config[CONF_FORCE_PARK])
        cg.add(b.set_parent(parent))
    if CONF_FORCE_RELEASE in config:
        b = await button.new_button(config[CONF_FORCE_RELEASE])
        cg.add(b.set_parent(parent))
    if CONF_STOP in config:
        b = await button.new_button(config[CONF_STOP])
        cg.add(b.set_parent(parent))
