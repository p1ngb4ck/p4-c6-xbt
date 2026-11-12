import os
import re
import sys
from pathlib import Path

path = Path(os.environ["IDF_PATH"]) / "components/bt/common/osi/fixed_queue.c"
text = path.read_text(encoding="utf-8")
done = re.compile(
    r"status = list_append\(queue->list, data\);[^\n]*\n"
    r"\s*if \(status == true\) \{\n"
    r"\s*osi_sem_give\(&queue->dequeue_sem\);\n"
    r"\s*\}\n"
    r"\s*osi_mutex_unlock\(&queue->lock\);"
)
if done.search(text):
    sys.exit(0)
pattern = re.compile(
    r"(?P<indent>[ \t]*)status = list_append\(queue->list, data\);(?P<tail>[^\n]*)\n"
    r"[ \t]*osi_mutex_unlock\(&queue->lock\);\n"
    r"\s*if\s*\(\s*status == true\s*\)\s*\n?"
    r"\s*osi_sem_give\(&queue->dequeue_sem\);"
)
if len(pattern.findall(text)) != 1:
    sys.exit(f"{path}: fixed_queue_enqueue's append / unlock / give sequence not found exactly once")


def replace(match):
    indent = match.group("indent")
    return (
        f"{indent}status = list_append(queue->list, data);{match.group('tail')}\n"
        f"{indent}if (status == true) {{\n"
        f"{indent}    osi_sem_give(&queue->dequeue_sem);\n"
        f"{indent}}}\n"
        f"{indent}osi_mutex_unlock(&queue->lock);"
    )


path.write_text(pattern.sub(replace, text), encoding="utf-8")
print(f"patched {path}: fixed_queue_enqueue gives dequeue_sem before it releases the queue lock")
