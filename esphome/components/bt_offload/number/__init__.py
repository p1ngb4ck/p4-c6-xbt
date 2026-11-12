import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG, UNIT_MILLISECOND

from .. import BTOffload, bt_offload_ns

DEPENDENCIES = ["bt_offload"]

CONF_BT_OFFLOAD_ID = "bt_offload_id"

BTOffloadDelayNumber = bt_offload_ns.class_(
    "BTOffloadDelayNumber", number.Number, cg.Parented.template(BTOffload)
)

MAX_DELAY_MS = 1600
MIN_DELAY_MS = -800

CONFIG_SCHEMA = number.number_schema(
    BTOffloadDelayNumber,
    unit_of_measurement=UNIT_MILLISECOND,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon="mdi:timer-sand",
).extend({cv.GenerateID(CONF_BT_OFFLOAD_ID): cv.use_id(BTOffload)})


async def to_code(config):
    var = await number.new_number(config, min_value=MIN_DELAY_MS, max_value=MAX_DELAY_MS, step=10)
    parent = await cg.get_variable(config[CONF_BT_OFFLOAD_ID])
    await cg.register_parented(var, parent)
    cg.add(parent.set_delay_number(var))
    cg.add_define("USE_BT_OFFLOAD_NUMBER")
