import os
import re
import sys
from pathlib import Path

bt = Path(os.environ["IDF_PATH"]) / "components/bt"

path = bt / "common/include/bt_user_config.h"
text = path.read_text(encoding="utf-8")
pattern = re.compile(r"(#ifdef CONFIG_BT_CONTROLLER_ENABLED\s*\n#define UC_BT_CONTROLLER_INCLUDED\s+)TRUE")
if pattern.search(text):
    path.write_text(pattern.sub(r"\1FALSE", text, count=1), encoding="utf-8")
    print(f"patched {path}: Bluedroid on the external HCI driver with the controller built in")
elif not re.search(r"#ifdef CONFIG_BT_CONTROLLER_ENABLED\s*\n#define UC_BT_CONTROLLER_INCLUDED\s+FALSE", text):
    sys.exit(f"{path}: UC_BT_CONTROLLER_INCLUDED under CONFIG_BT_CONTROLLER_ENABLED not found")

path = bt / "host/bluedroid/Kconfig.in"
text = path.read_text(encoding="utf-8")
if re.search(r"^\s*(config|select)\s+BT_CONTROLLER_DISABLED\b", text, re.M):
    sys.exit(f"{path}: BT_CONTROLLER_DISABLED defined or selected here")
count = len(re.findall(r"\bBT_CONTROLLER_DISABLED\b", text))
if count:
    path.write_text(re.sub(r"\bBT_CONTROLLER_DISABLED\b", "y", text), encoding="utf-8")
    print(f"patched {path}: {count} host options as with the controller disabled")
