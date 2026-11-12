import os
import re
import sys
from pathlib import Path

path = Path(os.environ["IDF_PATH"]) / "components/bt/host/bluedroid/stack/include/stack/bt_types.h"
text = path.read_text(encoding="utf-8")
helper = """#include <stddef.h>
static inline size_t bcm_strlcpy_s(char *dst, const char *src, size_t size)
{
    size_t n = 0;
    if (size == 0) {
        return 0;
    }
    while (n + 1 < size && src[n] != '\\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\\0';
    return n;
}
"""
if "bcm_strlcpy_s" in text:
    sys.exit(0)
pattern = re.compile(r"^#define[ \t]+(BCM_STRLCPY_S|BCM_STRNCPY_S)\([^)]*\)[ \t]+(strlcpy|strncpy)\([^\n]*\n", re.M)
matches = pattern.findall(text)
if not matches:
    sys.exit(f"{path}: neither BCM_STRLCPY_S nor BCM_STRNCPY_S found")
first = True


def replace(match):
    global first
    prefix = helper if first else ""
    first = False
    return f"{prefix}#define {match.group(1)}(x1,x2,x3)  bcm_strlcpy_s((char *)(x1),(const char *)(x2),(x3))\n"


path.write_text(pattern.sub(replace, text), encoding="utf-8")
print(f"patched {path}: {', '.join(m[0] for m in matches)}")
