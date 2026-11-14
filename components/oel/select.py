"""A select listing the clips in the build, plus "cycle" and "off"; or, with
type: aspect, how a clip meets the screen.

    select:
      - platform: oel
        name: Dashboard clip
      - platform: oel
        type: aspect
        name: Dashboard clip aspect
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.core import CORE

from . import ASPECTS, CONF_OEL_ID, OelComponent, clip_names, oel_ns

DEPENDENCIES = ["oel"]

OelSelect = oel_ns.class_("OelSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(OelSelect, icon="mdi:dolphin")
    .extend(
        {
            cv.GenerateID(CONF_OEL_ID): cv.use_id(OelComponent),
            cv.Optional("type", default="clip"): cv.one_of("clip", "aspect", lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OEL_ID])
    if config["type"] == "aspect":
        var = await select.new_select(config, options=ASPECTS)
        cg.add(var.set_aspect_select())
    else:
        var = await select.new_select(config, options=["cycle"] + clip_names(CORE.config["oel"]) + ["off"])
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
