# Building the firmware

One script builds everything: the main image, the minimal image, the factory image for serial flashing and the
manifest the update reads.

## Requirements

| Requirement | Value |
|---|---|
| Host | Linux with `bash`, `python3`, `sha256sum` |
| ESP-IDF | 6.2.0, the copy ESPHome installs: `~/.cache/esphome/idf/frameworks/6.2.0` with its tools in `~/.cache/esphome/idf` |
| esp_hosted | 3.1.0, fetched by the build from the ESP component registry |
| Network | needed once per build for the esp_hosted example project and its components |

The ESP-IDF copy appears on a machine after ESPHome has compiled an ESP32 configuration with this framework block
(ESP-IDF 6.2.0 has no release tag yet, it is the `master` branch):

```yaml
esp32:
  framework:
    type: esp-idf
    version: 6.2.0
    source: https://github.com/espressif/esp-idf.git@master
```

The firmware is developed against ESP-IDF commit `d445e947`. The build does not change the ESP-IDF copy: the
Bluetooth host and HID sources are copied into the build folder and patched there.

Why this version and not the ESP-IDF 5.5 that ESPHome uses by default: the patches change ESP-IDF's own Bluetooth
host sources and are written against the sources of 6.2.0. Each patch looks for the place it changes and stops the
build with a message when it does not find it, so another ESP-IDF version does not build silently wrong; it needs
the patches checked first. The P4 side is built on the same version
([esphome-config.md](esphome-config.md#framework)).

The esp_hosted version of the firmware must not be newer than the esp_hosted version of the ESPHome build on the P4.
ESPHome refuses to install a coprocessor image that is newer than its own host library.

## Build

```bash
git clone https://github.com/p1ngb4ck/p4-c6-xbt..git c6/src
XBT_FW_BUILD=1 bash c6/src/scripts/build_local.sh
```

| Variable | Meaning | Default |
|---|---|---|
| `XBT_FW_BUILD` | Build number, required. It becomes the last part of the version: `3.1.0+xbt.1`. Raise it for every build you want to install over an older one | none |
| `ESP_HOSTED_VERSION` | esp_hosted release the firmware is built on | `3.1.0` |

> [!IMPORTANT]
> The script works in the folder that contains the clone. It deletes and recreates `work/` and `out/` beside the
> clone (`c6/work`, `c6/out` in the example) and keeps the ELF files in `c6/elf/`. Clone into a folder of its own.

Both images are built from scratch every time. The script prints `STEP` lines (`create project`, `components`, `patches`, `build`,
`build minimal`, `collect`) and ends with `STEP done xbt.<number>`.

## What a build produces

All files are in `out/`.

| File | What it is | Used for |
|---|---|---|
| `network_adapter_esp32c6_bt-offload.bin` | Main image (Wi-Fi and Bluetooth host) | Update over the air |
| `network_adapter_esp32c6_minimal.bin` | Minimal image (Wi-Fi and update only) | Update over the air: first installation from the stock firmware, and renewed when its revision rises |
| `network_adapter_esp32c6_bt-offload.factory.bin` | Bootloader, partition table, main and minimal image in one file, written at offset 0 | Serial flashing |
| `manifest_bt-offload.json` | Version, file names and SHA256 of the images | The update's `source` |
| `network_adapter_esp32c6_bt-offload.bin.sha256` | SHA256 of the main image | Checking a download |
| `bootloader.bin`, `partition-table.bin`, `flash_args` | The single parts of the factory image | Flashing with `esptool` part by part |
| `sdkconfig_bt-offload` | The build options the main image was built with | Reference |

The ELF file of the main image is kept as `elf/xbt.<number>.elf`. It is needed to read a crash record of that
build.

### The manifest

```json
{"versions": [{
  "version": "3.1.0+xbt.1",
  "url": "network_adapter_esp32c6_bt-offload.bin",
  "sha256": "...",
  "minimal_url": "network_adapter_esp32c6_minimal.bin",
  "minimal_sha256": "...",
  "minimal_revision": 1
}]}
```

| Field | Meaning |
|---|---|
| `version` | esp_hosted version, `+xbt.` and the build number. The `xbt` part tells ESPHome that this is this firmware and not the stock one |
| `url`, `sha256` | Main image. A name without a folder is looked up beside the manifest |
| `minimal_url`, `minimal_sha256` | Minimal image. Required for the switch from the stock firmware |
| `minimal_revision` | Revision of the minimal image. A device with an older minimal image gets the new one before the main image |
| `factory_url`, `factory_sha256` | Optional, not written by the build: the factory image. When present and the serial route is configured, a failed update over the air is repaired over serial by itself |

To add the factory image, put its file name and SHA256 into the manifest entry:

```bash
sha256sum out/network_adapter_esp32c6_bt-offload.factory.bin
```

Next: [flashing.md](flashing.md).
