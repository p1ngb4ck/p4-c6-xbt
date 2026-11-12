import os
from pathlib import Path
import re
import sys

root = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/stack/l2cap"

path = root / "l2c_utils.c"
text = path.read_text(encoding="utf-8")
old = ("    p_ccb->fcrb.srej_rcv_hold_q = fixed_queue_new(QUEUE_SIZE_MAX);\n"
       "    p_ccb->fcrb.retrans_q = fixed_queue_new(QUEUE_SIZE_MAX);\n"
       "    p_ccb->fcrb.waiting_for_ack_q = fixed_queue_new(QUEUE_SIZE_MAX);\n")
new = ("    p_ccb->fcrb.srej_rcv_hold_q = NULL;\n"
       "    p_ccb->fcrb.retrans_q = NULL;\n"
       "    p_ccb->fcrb.waiting_for_ack_q = NULL;\n")
if text.count(old) != 1:
    sys.exit(f"{path}: ERTM queue creation at channel allocation not found ({text.count(old)})")
path.write_text(text.replace(old, new), encoding="utf-8")

path = root / "l2c_fcr.c"
text = path.read_text(encoding="utf-8")
anchor = '#include "osi/allocator.h"\n'
helper = ("\nstatic void l2c_fcr_enqueue(fixed_queue_t **queue, void *data)\n"
          "{\n"
          "    if (*queue == NULL) {\n"
          "        *queue = fixed_queue_new(QUEUE_SIZE_MAX);\n"
          "    }\n"
          "    fixed_queue_enqueue(*queue, data, FIXED_QUEUE_MAX_TIMEOUT);\n"
          "}\n")
if text.count(anchor) != 1:
    sys.exit(f"{path}: include anchor not found ({text.count(anchor)})")
text = text.replace(anchor, anchor + helper)
text, count = re.subn(r"fixed_queue_enqueue\((p_fcrb->|p_ccb->fcrb\.)(srej_rcv_hold_q|retrans_q|waiting_for_ack_q), (\w+), "
                      r"FIXED_QUEUE_MAX_TIMEOUT\)", r"l2c_fcr_enqueue(&\1\2, \3)", text)
if count != 5:
    sys.exit(f"{path}: {count} ERTM queue enqueue sites, expected 5")
old = "    p_buf = (BT_HDR *)fixed_queue_dequeue(p_ccb->fcrb.retrans_q, 0);\n"
new = ("    p_buf = p_ccb->fcrb.retrans_q != NULL ? (BT_HDR *)fixed_queue_dequeue(p_ccb->fcrb.retrans_q, 0) : NULL;\n")
if text.count(old) != 1:
    sys.exit(f"{path}: retransmit queue read not found ({text.count(old)})")
path.write_text(text.replace(old, new), encoding="utf-8")
print(f"patched {root}: the three ERTM queues of an L2CAP channel are created at their first use")
