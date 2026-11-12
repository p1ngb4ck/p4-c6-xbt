import os
from pathlib import Path
import sys

path = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/bta/gatt/bta_gattc_utils.c"
text = path.read_text(encoding="utf-8")
old = ("            /* clean up cache */\n"
       "            if (p_srcb->p_srvc_cache) {\n"
       "                list_free(p_srcb->p_srvc_cache);\n"
       "                p_srcb->p_srvc_cache = NULL;\n"
       "            }\n")
count = text.count(old)
if count == 1:
    path.write_text(text.replace(old, ""), encoding="utf-8")
    print(f"patched {path}: a known server keeps its GATT cache in RAM after the disconnect until its slot is reused")
elif count != 0 or "/* clean up cache */" in text:
    sys.exit(f"{path}: GATT cache release on disconnect not found ({count})")
