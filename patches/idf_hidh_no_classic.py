import os
from pathlib import Path
import sys

path = Path(os.environ["IDF_PATH"]) / "components/esp_hid/src/esp_hidh.c"
text = path.read_text(encoding="utf-8")
patched = text
for call in ("esp_bt_hidh_init(config),", "esp_bt_hidh_deinit(),"):
    patched = patched.replace(call, "ESP_OK,")
patched = patched.replace("    esp_bt_hidh_deinit();\n", "")
if patched != text:
    path.write_text(patched, encoding="utf-8")
    print(f"patched {path}: Classic HID host left to the caller, BLE stays in esp_hidh")
elif '"BT HIDH init failed"' not in text:
    sys.exit(f"{path}: Classic HID host init not found")
