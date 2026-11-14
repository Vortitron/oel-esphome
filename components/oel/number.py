"""A 1-bit panel's cutoff as a slider: shades dimmer than it stay dark instead
of being dithered. Kept over a restart unless restore_value: false.

    number:
      - platform: oel
        name: Dashboard clip cutoff
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_RESTORE_VALUE, ENTITY_CATEGORY_CONFIG, UNIT_PERCENT

from . import CONF_OEL_ID, OelComponent, oel_ns

DEPENDENCIES = ["oel"]

OelNumber = oel_ns.class_("OelNumber", number.Number, cg.Component)

CONFIG_SCHEMA = (
    number.number_schema(
        OelNumber, icon="mdi:contrast-box", unit_of_measurement=UNIT_PERCENT, entity_category=ENTITY_CATEGORY_CONFIG
    )
    .extend(
        {
            cv.GenerateID(CONF_OEL_ID): cv.use_id(OelComponent),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OEL_ID])
    var = await number.new_number(config, min_value=0, max_value=100, step=5)
    await cg.register_component(var, config)
    cg.add(var.set_parent(parent))
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
