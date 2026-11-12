import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_UPDATE_INTERVAL,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_BYTES,
    UNIT_MILLISECOND,
    UNIT_SECOND,
)

from .. import BTOffload

DEPENDENCIES = ["bt_offload"]

CONF_BT_OFFLOAD_ID = "bt_offload_id"

FIELDS = {
    "uptime": (0, UNIT_SECOND, "mdi:timer-outline", STATE_CLASS_TOTAL_INCREASING, 0),
    "internal_free": (1, UNIT_BYTES, "mdi:memory", STATE_CLASS_MEASUREMENT, 0),
    "internal_min_free": (2, UNIT_BYTES, "mdi:memory", STATE_CLASS_MEASUREMENT, 0),
    "internal_largest_block": (3, UNIT_BYTES, "mdi:memory", STATE_CLASS_MEASUREMENT, 0),
    "tasks": (4, None, "mdi:format-list-numbered", STATE_CLASS_MEASUREMENT, 0),
    "worker_stack_free": (5, UNIT_BYTES, "mdi:layers-outline", STATE_CLASS_MEASUREMENT, 0),
    "audio_ring_free": (6, UNIT_BYTES, "mdi:music-note", STATE_CLASS_MEASUREMENT, 0),
    "a2dp_calls": (7, None, "mdi:music-note", STATE_CLASS_MEASUREMENT, 0),
    "a2dp_max": (8, UNIT_MILLISECOND, "mdi:timer-sand", STATE_CLASS_MEASUREMENT, 2),
    "command_max": (9, UNIT_MILLISECOND, "mdi:timer-sand", STATE_CLASS_MEASUREMENT, 2),
    "frames_in": (10, None, "mdi:download", STATE_CLASS_MEASUREMENT, 0),
    "frames_out": (11, None, "mdi:upload", STATE_CLASS_MEASUREMENT, 0),
    "send_max": (12, UNIT_MILLISECOND, "mdi:timer-sand", STATE_CLASS_MEASUREMENT, 2),
    "a2dp_underruns": (13, None, "mdi:music-note-off", STATE_CLASS_MEASUREMENT, 0),
    "reset_reason": (14, None, "mdi:restart", None, 0),
    "waiting_messages": (16, None, "mdi:tray-full", STATE_CLASS_MEASUREMENT, 0),
    "crashed_starts": (17, None, "mdi:alert-circle-outline", STATE_CLASS_MEASUREMENT, 0),
    "waiting_h4": (18, None, "mdi:tray-full", STATE_CLASS_MEASUREMENT, 0),
}


def _field_schema(unit, icon, state_class, accuracy):
    kwargs = {
        "icon": icon,
        "accuracy_decimals": accuracy,
        "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
    }
    if unit is not None:
        kwargs["unit_of_measurement"] = unit
    if state_class is not None:
        kwargs["state_class"] = state_class
    return sensor.sensor_schema(**kwargs)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BT_OFFLOAD_ID): cv.use_id(BTOffload),
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
        **{
            cv.Optional(name): _field_schema(unit, icon, state_class, accuracy)
            for name, (_, unit, icon, state_class, accuracy) in FIELDS.items()
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BT_OFFLOAD_ID])
    cg.add_define("USE_BT_OFFLOAD_SENSOR")
    cg.add(parent.set_status_interval(config[CONF_UPDATE_INTERVAL]))
    for name, (field, *_rest) in FIELDS.items():
        if name in config:
            sens = await sensor.new_sensor(config[name])
            cg.add(parent.set_status_sensor(field, sens))
