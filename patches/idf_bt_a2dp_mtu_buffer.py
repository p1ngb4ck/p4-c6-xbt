import os
from pathlib import Path
import sys

path = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c"
text = path.read_text(encoding="utf-8")
old = "if (NULL == (p_buf = osi_malloc(BTC_MEDIA_AA_BUF_SIZE))) {"
new = ("if (NULL == (p_buf = osi_malloc(sizeof(BT_HDR) + BTC_MEDIA_AA_SBC_OFFSET + "
       "a2dp_source_local_param.btc_aa_src_cb.TxAaMtuSize))) {")
count = text.count(old)
if count == 1:
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched {path}: an A2DP media packet takes the header, the SBC offset and the link's MTU, not 4112 B")
elif new not in text:
    sys.exit(f"{path}: A2DP media packet allocation not found ({count})")
