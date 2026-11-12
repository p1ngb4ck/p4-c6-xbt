from pathlib import Path
import re
import sys

root = Path(sys.argv[1]) / "managed_components/espressif__esp_hosted/coprocessor"
edits = [
    ("features/eh_cp_feat_rpc/src/eh_cp_feat_rpc_ll.c",
     r'(xTaskCreate\(slow_req_worker,\s*"eh_rpc_slow",\s*)CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE', 5120),
    ("features/eh_cp_feat_rpc/src/eh_cp_feat_rpc_ll.c",
     r'(xTaskCreate\(pserial_task,\s*"pserial_task",\s*)CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE', 5120),
]
for rel, pattern, size in edits:
    path = root / rel
    text = path.read_text(encoding="utf-8")
    patched, count = re.subn(pattern, r"\g<1>" + str(size), text)
    if count != 1:
        sys.exit(f"{path}: task creation not found for {pattern}")
    path.write_text(patched, encoding="utf-8")
    print(f"patched {path}: stack {size}")
