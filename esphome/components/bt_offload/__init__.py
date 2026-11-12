from esphome import automation
import esphome.codegen as cg
from esphome.components.bt_host.pairing import DevicePicker
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import CONF_CODE, CONF_ID, CONF_PIN, CONF_TRIGGER_ID
import esphome.final_validate as fv

DEPENDENCIES = ["esp32", "esp32_hosted", "usb_bluetooth"]
AUTO_LOAD = ["hid", "bt_host"]

bt_offload_ns = cg.esphome_ns.namespace("bt_offload")
BTOffload = bt_offload_ns.class_("BTOffload", cg.Component, HIDInput, DevicePicker)
StatusTrigger = bt_offload_ns.class_("StatusTrigger", automation.Trigger.template(cg.std_string))
ConnectedTrigger = bt_offload_ns.class_("ConnectedTrigger", automation.Trigger.template(cg.std_string))
CodeRequestTrigger = bt_offload_ns.class_(
    "CodeRequestTrigger", automation.Trigger.template(cg.std_string, cg.bool_, cg.std_string)
)
PairReplyAction = bt_offload_ns.class_("PairReplyAction", automation.Action)
PairCancelAction = bt_offload_ns.class_("PairCancelAction", automation.Action)
_DATA = cg.std_vector.template(cg.uint8)
ManufacturerDataTrigger = bt_offload_ns.class_(
    "ManufacturerDataTrigger", automation.Trigger.template(_DATA, cg.uint16, cg.std_string)
)
ServiceDataTrigger = bt_offload_ns.class_(
    "ServiceDataTrigger", automation.Trigger.template(_DATA, cg.uint16, cg.std_string)
)

PackageManager = cg.esphome_ns.namespace("package_manager").class_("PackageManager", cg.Component)

CONF_PACKAGE_MANAGER_ID = "package_manager_id"
CONF_MAX_HID_DEVICES = "max_hid_devices"
CONF_C6_LOG_LEVEL = "c6_log_level"
C6_LOG_LEVELS = {"NONE": 0, "ERROR": 1, "WARN": 2, "INFO": 3, "DEBUG": 4, "VERBOSE": 5}
CONF_ON_STATUS = "on_status"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_CODE_REQUEST = "on_code_request"
CONF_ON_MANUFACTURER_DATA = "on_manufacturer_data"
CONF_ON_SERVICE_DATA = "on_service_data"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BTOffload),
            cv.OnlyWith(CONF_PACKAGE_MANAGER_ID, "package_manager"): cv.use_id(PackageManager),
            cv.Optional(CONF_MAX_HID_DEVICES, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_PIN, default=""): cv.All(cv.string, cv.Length(max=16)),
            cv.Optional(CONF_C6_LOG_LEVEL): cv.enum(C6_LOG_LEVELS, upper=True),
            cv.Optional(CONF_ON_CODE_REQUEST): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CodeRequestTrigger)}
            ),
            cv.Optional(CONF_ON_STATUS): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StatusTrigger)}
            ),
            cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConnectedTrigger)}
            ),
            cv.Optional(CONF_ON_MANUFACTURER_DATA): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ManufacturerDataTrigger)}
            ),
            cv.Optional(CONF_ON_SERVICE_DATA): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ServiceDataTrigger)}
            ),
        }
    )
    .extend(HID_INPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    dongles = fv.full_config.get().get("usb_bluetooth") or []
    if not any(d.get("c6_offload") for d in dongles):
        raise cv.Invalid("bt_offload needs a usb_bluetooth with c6_offload: true")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_hid_input(var, config)
    cg.add(var.set_max_hid_devices(config[CONF_MAX_HID_DEVICES]))
    cg.add_define("USE_BT_OFFLOAD")
    cg.add_build_flag("-DESP_TRANSPORT_SDIO_MAX_BUF_SIZE=2048")
    cg.add(var.set_pin(config[CONF_PIN]))
    if CONF_C6_LOG_LEVEL in config:
        cg.add(var.set_c6_log_level(config[CONF_C6_LOG_LEVEL]))
    if CONF_PACKAGE_MANAGER_ID in config:
        cg.add_define("USE_BT_OFFLOAD_PACKAGE_MANAGER")
        package_manager = await cg.get_variable(config[CONF_PACKAGE_MANAGER_ID])
        cg.add(var.set_package_manager(package_manager))
    for conf in config.get(CONF_ON_CODE_REQUEST, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "device"), (cg.bool_, "pin"), (cg.std_string, "preset")],
            conf,
        )
    for conf in config.get(CONF_ON_STATUS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)
    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "device")], conf)
    for conf in config.get(CONF_ON_MANUFACTURER_DATA, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(_DATA, "x"), (cg.uint16, "id"), (cg.std_string, "device")], conf
        )
    for conf in config.get(CONF_ON_SERVICE_DATA, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(_DATA, "x"), (cg.uint16, "uuid"), (cg.std_string, "device")], conf
        )


@automation.register_action(
    "bt_offload.pair_reply",
    PairReplyAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BTOffload),
            cv.Required(CONF_CODE): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def bt_offload_pair_reply_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    code = await cg.templatable(config[CONF_CODE], args, cg.std_string)
    cg.add(var.set_code(code))
    return var


@automation.register_action(
    "bt_offload.pair_cancel",
    PairCancelAction,
    cv.Schema({cv.GenerateID(): cv.use_id(BTOffload)}),
    synchronous=True,
)
async def bt_offload_pair_cancel_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
