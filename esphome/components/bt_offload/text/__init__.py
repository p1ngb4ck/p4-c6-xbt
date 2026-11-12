import esphome.codegen as cg
from esphome.components import text
from esphome.components.bt_offload import BTOffload, bt_offload_ns
import esphome.config_validation as cv
from esphome.const import CONF_TYPE

DEPENDENCIES = ["bt_offload"]

CONF_BT_OFFLOAD_ID = "bt_offload_id"
TYPE_PRESET = "preset_pin"
TYPE_CODE = "pair_code"

BTOffloadText = bt_offload_ns.class_("BTOffloadText", text.Text, cg.Component)

CONFIG_SCHEMA = (
    text.text_schema(BTOffloadText, mode="TEXT")
    .extend(
        {
            cv.GenerateID(CONF_BT_OFFLOAD_ID): cv.use_id(BTOffload),
            cv.Required(CONF_TYPE): cv.one_of(TYPE_PRESET, TYPE_CODE, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text.new_text(config, min_length=0, max_length=16)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BT_OFFLOAD_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_code_entry(config[CONF_TYPE] == TYPE_CODE))
