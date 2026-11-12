#!/bin/bash
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
BASE=$(dirname "$SRC")
WORK=$BASE/work
OUT=$BASE/out
: "${XBT_FW_BUILD:?}"
export XBT_FW_BUILD
export ESP_HOSTED_VERSION=${ESP_HOSTED_VERSION:-3.1.0}
export IDF_PATH=$HOME/.cache/esphome/idf/frameworks/6.2.0
export IDF_TOOLS_PATH=$HOME/.cache/esphome/idf
export IDF_PYTHON_ENV_PATH=$HOME/.cache/esphome/idf/penvs/6.2.0
. "$IDF_PATH/export.sh" > /dev/null

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"
cd "$WORK"

echo "STEP create project"
idf.py create-project-from-example --path coprocessor "espressif/esp_hosted==${ESP_HOSTED_VERSION}:wifi/sta/cp"
python3 - coprocessor/main/idf_component.yml <<'EOF'
import os, re, sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
pinned, count = re.subn(r"(espressif/esp_hosted:\s*\n\s*version:\s*)\S+", r"\g<1>'==" + os.environ["ESP_HOSTED_VERSION"] + "'", text)
if count != 1:
    sys.exit(f"{path}: espressif/esp_hosted version not found")
path.write_text(pinned, encoding="utf-8")
print(f"pinned {path}: espressif/esp_hosted ==" + os.environ["ESP_HOSTED_VERSION"])
EOF

echo "STEP components"
mkdir -p coprocessor/components
cp -r "$SRC"/components/* coprocessor/components/
cp -r "$IDF_PATH/components/bt" "$IDF_PATH/components/esp_hid" coprocessor/components/
cp -r coprocessor minimal
for f in coprocessor/sdkconfig.defaults coprocessor/sdkconfig.defaults.esp32c6; do
  printf '\n' >> "$f"
  cat "$SRC/variants/offload.sdkconfig" >> "$f"
done
for f in minimal/sdkconfig.defaults minimal/sdkconfig.defaults.esp32c6; do
  printf '\n' >> "$f"
  printf '%s\n' \
    'CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y' \
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' \
    'CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y' \
    'CONFIG_COMPILER_OPTIMIZATION_SIZE=y' \
    'CONFIG_ESP_ERR_TO_NAME_LOOKUP=n' \
    'CONFIG_LOG_DEFAULT_LEVEL_WARN=y' \
    'CONFIG_LOG_MAXIMUM_LEVEL_WARN=y' >> "$f"
done
for f in coprocessor/sdkconfig.defaults coprocessor/sdkconfig.defaults.esp32c6 minimal/sdkconfig.defaults minimal/sdkconfig.defaults.esp32c6; do
  printf '%s\n' \
    'CONFIG_PARTITION_TABLE_CUSTOM=y' \
    'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_xbt_4m.csv"' \
    'CONFIG_PARTITION_TABLE_FILENAME="partitions_xbt_4m.csv"' >> "$f"
done
printf '%s\n' \
  'nvs,      data, nvs,     0x9000,   0x4000' \
  'otadata,  data, ota,     0xd000,   0x2000' \
  'phy_init, data, phy,     0xf000,   0x1000' \
  'ota_0,    app,  ota_0,   0x10000,  0x290000' \
  'coredump, data, coredump, 0x2A0000, 0x10000' \
  'ota_1,    app,  ota_1,   0x2B0000, 0x150000' > coprocessor/partitions_xbt_4m.csv
cp coprocessor/partitions_xbt_4m.csv minimal/partitions_xbt_4m.csv
python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" -q --flash-size 4MB \
  minimal/partitions_xbt_4m.csv minimal/components/c6_xbt/table_xbt.bin
python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" -q --flash-size 4MB \
  minimal/partitions_eh_cp_ota_4m.csv minimal/components/c6_xbt/table_stock.bin

echo "STEP patches"
for project in coprocessor minimal; do
  grep -rlE '\$\{?IDF_PATH\}?/components/(bt|esp_hid)/' "$WORK/$project/components/bt" "$WORK/$project/components/esp_hid" \
    | xargs -r sed -i -E 's#\$\{?IDF_PATH\}?/components/(bt|esp_hid)/#'"$WORK/$project"'/components/\1/#g'
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_strlcpy.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_controller_route.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_hidh_no_classic.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_simple_pairing.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_a2dp_mtu_buffer.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_gattc_keep_cache.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_gattc_notify_copy.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_fixed_queue_atomic.py"
  IDF_PATH="$WORK/$project" python3 "$SRC/patches/idf_bt_l2c_ertm_queues.py"
done

echo "STEP build"
cd "$WORK/coprocessor"
idf.py set-target esp32c6
python3 "$SRC/patches/hosted_fw_build.py" .
python3 "$SRC/patches/hosted_ota_guard.py" .
python3 "$SRC/patches/hosted_task_stacks.py" .
python3 - managed_components/espressif__esp_hosted/coprocessor/eh_cp_transport/src/eh_cp_transport_sdio.c <<'EOF'
import re, sys
from pathlib import Path
path = Path(sys.argv[1])
text, count = re.subn(r"(#define SDIO_RX_BUFFER_NUM\s+)2\b", r"\g<1>4", path.read_text(encoding="utf-8"))
if count != 1:
    sys.exit(f"{path}: SDIO_RX_BUFFER_NUM 2 not found")
text, count = re.subn(r"sdio_rx_buf_size = sdio_hw_max_rx_buf_size\(\);",
                      "sdio_rx_buf_size = 3u * MAX_TRANSPORT_BUF_SIZE < sdio_hw_max_rx_buf_size() ? "
                      "3u * MAX_TRANSPORT_BUF_SIZE : sdio_hw_max_rx_buf_size();", text)
if count != 1:
    sys.exit(f"{path}: SW_AGGR buffer size not found")
path.write_text(text, encoding="utf-8")
root = path.parents[3]
for name in ("coprocessor/eh_cp_transport/include/common/eh_transport.h", "common/eh_common/include/eh_common_interface.h"):
    header = root / name
    text, count = re.subn(r"(#define ESP_TRANSPORT_SDIO_MAX_BUF_SIZE\s+)1536\b", r"\g<1>2048", header.read_text(encoding="utf-8"))
    if count != 1:
        sys.exit(f"{header}: ESP_TRANSPORT_SDIO_MAX_BUF_SIZE 1536 not found")
    header.write_text(text, encoding="utf-8")
print(f"patched {path}: SDIO packet 2048 B, SDIO_RX_BUFFER_NUM 4, SW_AGGR buffers 3 frames (6144 B) each")
EOF
idf.py build
idf.py size

echo "STEP build minimal"
cd "$WORK/minimal"
idf.py set-target esp32c6
python3 "$SRC/patches/hosted_fw_build.py" .
idf.py build
idf.py size

echo "STEP collect"
cd "$WORK"
NAME=network_adapter_esp32c6_bt-offload.bin
cp coprocessor/build/eh_cp_wifi_sta.bin "$OUT/$NAME"
mkdir -p "$BASE/elf"
cp coprocessor/build/eh_cp_wifi_sta.elf "$BASE/elf/xbt.$XBT_FW_BUILD.elf"
cp coprocessor/sdkconfig "$OUT/sdkconfig_bt-offload"
cp coprocessor/build/bootloader/bootloader.bin "$OUT/bootloader.bin"
cp coprocessor/build/partition_table/partition-table.bin "$OUT/partition-table.bin"
cp coprocessor/build/flash_args "$OUT/flash_args"
cp minimal/build/eh_cp_wifi_sta.bin "$OUT/network_adapter_esp32c6_minimal.bin"
(cd coprocessor/build && python -m esptool --chip esp32c6 merge-bin -o "$OUT/network_adapter_esp32c6_bt-offload.factory.bin" @flash_args 0x2B0000 ../../minimal/build/eh_cp_wifi_sta.bin)
SHA=$(sha256sum "$OUT/$NAME" | cut -d' ' -f1)
echo "$SHA  $NAME" > "$OUT/$NAME.sha256"
MINIMAL=network_adapter_esp32c6_minimal.bin
MINIMAL_SHA=$(sha256sum "$OUT/$MINIMAL" | cut -d' ' -f1)
MINIMAL_REVISION=$(sed -n 's/^#define XBT_MINIMAL_REVISION \([0-9][0-9]*\).*/\1/p' "$SRC/components/c6_xbt/c6_xbt_proto.h")
: "${MINIMAL_REVISION:?}"
printf '{"versions": [{"version": "%s+xbt.%s", "url": "%s", "sha256": "%s", "minimal_url": "%s", "minimal_sha256": "%s", "minimal_revision": %s}]}\n' \
  "$ESP_HOSTED_VERSION" "$XBT_FW_BUILD" "$NAME" "$SHA" "$MINIMAL" "$MINIMAL_SHA" "$MINIMAL_REVISION" > "$OUT/manifest_bt-offload.json"
echo "STEP done xbt.$XBT_FW_BUILD"
