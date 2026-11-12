import sys
from pathlib import Path

path = Path(sys.argv[1]) / "managed_components/espressif__esp_hosted/coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_system.c"
text = path.read_text(encoding="utf-8")
if "OTA refused in the main firmware" in text:
    sys.exit(0)
old = '\tESP_LOGI(TAG, "Prepare partition for OTA\\n");\n'
if text.count(old) != 1:
    sys.exit(f"{path}: OTA begin's 'Prepare partition' line found {text.count(old)} times")
guard = (
    "#if CONFIG_C6_XBT\n"
    '\tESP_LOGE(TAG, "OTA refused in the main firmware: it is written from the minimal firmware");\n'
    "\ts_ota_status = OTA_FAILED;\n"
    "\tret = ESP_ERR_NOT_SUPPORTED;\n"
    "\tgoto err;\n"
    "#endif\n"
)
path.write_text(text.replace(old, guard + old), encoding="utf-8")
print(f"patched {path}: no OTA begin while the main firmware runs")
