"""A select listing the clips in the build, plus "cycle" and "off".

    select:
      - platform: oel
        name: Dashboard clip
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.core import CORE

from . import CONF_OEL_ID, OelComponent, clip_names, oel_ns

DEPENDENCIES = ["oel"]

OelSelect = oel_ns.class_("OelSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(OelSelect, icon="mdi:dolphin")
    .extend({cv.GenerateID(CONF_OEL_ID): cv.use_id(OelComponent)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OEL_ID])
    var = await select.new_select(config, options=["cycle"] + clip_names(CORE.config["oel"]) + ["off"])
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
