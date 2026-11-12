import os
from pathlib import Path
import sys

root = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid"

api = root / "bta/include/bta/bta_gatt_api.h"
text = api.read_text(encoding="utf-8")
old = ("    UINT16              len;\n"
       "    UINT8               value[BTA_GATT_MAX_ATTR_LEN];\n"
       "    BOOLEAN             is_notify;\n"
       "} tBTA_GATTC_NOTIFY;\n")
new = ("    UINT16              len;\n"
       "    BOOLEAN             is_notify;\n"
       "    UINT8               value[BTA_GATT_MAX_ATTR_LEN];\n"
       "} tBTA_GATTC_NOTIFY;\n")
count = text.count(old)
if count == 1:
    api.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched {api}: is_notify before the value, so a notification can be copied up to its length")
elif new not in text:
    sys.exit(f"{api}: tBTA_GATTC_NOTIFY layout not found ({count})")

btc = root / "btc/profile/std/gatt/btc_gattc.c"
text = btc.read_text(encoding="utf-8")
old = ("    ret = btc_transfer_context(&msg, p_data, sizeof(tBTA_GATTC),\n"
       "                                btc_gattc_copy_req_data, btc_gattc_free_req_data);\n")
new = ("    int arg_len = sizeof(tBTA_GATTC);\n"
       "    if (event == BTA_GATTC_NOTIF_EVT && p_data != NULL && p_data->notify.len <= BTA_GATT_MAX_ATTR_LEN) {\n"
       "        arg_len = (int) (offsetof(tBTA_GATTC_NOTIFY, value) + p_data->notify.len);\n"
       "    }\n"
       "    ret = btc_transfer_context(&msg, p_data, arg_len,\n"
       "                                btc_gattc_copy_req_data, btc_gattc_free_req_data);\n")
count = text.count(old)
if count == 1:
    text = text.replace(old, new)
    if "#include <stddef.h>" not in text:
        text = text.replace("#include <string.h>\n", "#include <stddef.h>\n#include <string.h>\n", 1)
    btc.write_text(text, encoding="utf-8")
    print(f"patched {btc}: a notification goes to the BTC task with its header and its bytes, not the 532 B union")
elif "offsetof(tBTA_GATTC_NOTIFY, value)" not in text:
    sys.exit(f"{btc}: btc_gattc_cback transfer not found ({count})")
