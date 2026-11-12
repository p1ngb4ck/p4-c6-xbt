import os
from pathlib import Path
import re
import sys

path = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/stack/btm/btm_sec.c"
text = path.read_text(encoding="utf-8")
pattern = re.compile(
    r"evt_data\.auth_req = \(p_dev_rec->p_cur_service->security_flags &\s*"
    r"BTM_SEC_OUT_MITM\) \? BTM_AUTH_SP_YES : BTM_AUTH_SP_NO;"
)
patched, count = pattern.subn("evt_data.auth_req = BTM_AUTH_SPGB_NO;", text)
if count == 1:
    path.write_text(patched, encoding="utf-8")
    print(f"patched {path}: outgoing Classic pairing is plain simple pairing without MITM, general bonding")
elif "evt_data.auth_req = BTM_AUTH_SPGB_NO;" not in text:
    sys.exit(f"{path}: outgoing pairing auth_req not found")

target = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/common/include/common/bt_target.h"
text = target.read_text(encoding="utf-8")
default = re.compile(r"(#define BTM_DEFAULT_AUTH_REQ\s+)BTM_AUTH_SP_NO\b")
patched, count = default.subn(r"\g<1>BTM_AUTH_SPGB_NO", text)
if count == 1:
    target.write_text(patched, encoding="utf-8")
    print(f"patched {target}: a pairing the peer starts is answered with general bonding without MITM")
elif not re.search(r"#define BTM_DEFAULT_AUTH_REQ\s+BTM_AUTH_SPGB_NO\b", text):
    sys.exit(f"{target}: BTM_DEFAULT_AUTH_REQ not found")
