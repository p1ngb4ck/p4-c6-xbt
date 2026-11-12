import os
import sys
from pathlib import Path

build = int(os.environ["XBT_FW_BUILD"])
path = Path(sys.argv[1]) / "managed_components/espressif__esp_hosted/coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_system.c"
text = path.read_text(encoding="utf-8")
old = "resp_payload->build = -1;"
if text.count(old) != 1:
    sys.exit(f"{path}: '{old}' found {text.count(old)} times")
path.write_text(text.replace(old, f"resp_payload->build = {build};"), encoding="utf-8")
print(f"patched {path}: build {build}")
