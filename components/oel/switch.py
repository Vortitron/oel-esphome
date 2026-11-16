"""Frame-rate modulation on a 1-bit panel as a switch, to judge the shimmer
for yourself. It starts as the YAML says, and is kept over a restart once
changed unless restore_mode says otherwise.

    switch:
      - platform: oel
        name: Dashboard clip shading
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_OEL_ID, OelComponent, oel_ns

DEPENDENCIES = ["oel"]

OelSwitch = oel_ns.class_("OelSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = (
    switch.switch_schema(
        OelSwitch, icon="mdi:gradient-horizontal", entity_category=ENTITY_CATEGORY_CONFIG, block_inverted=True
    )
    .extend({cv.GenerateID(CONF_OEL_ID): cv.use_id(OelComponent)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OEL_ID])
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
