# The ESPHome side

The firmware in this repository runs on the C6. This folder holds the part that runs on the P4, so that everything
belonging to the firmware can be read in one place. It is a copy for reading; the components are developed in the
repositories linked below.

| Path | What it is | Developed in |
|---|---|---|
| `components/bt_offload/` | Our component: devices, pairing, HID input, the Bluetooth speaker, status values and the bond backup, all through messages to the C6 | [esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth) |
| `components/usb_bluetooth/` | Our component: USB Bluetooth dongles as HCI controllers. `c6_offload.cpp` is the tunnel between the dongle and the C6 | [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers) |
| `components/esp32_hosted/` | ESPHome's component with our changes to its `update` platform | [esphome-network](https://github.com/p1ngb4ck/esphome-network) |
| `esp32_hosted.patch` | Our changes to `esp32_hosted` as a diff against ESPHome `dev` at `428883ed80`: 6 files, 2119 lines added, 258 removed | - |

`bt_offload` and `usb_bluetooth` do not exist in upstream ESPHome; every line of them is part of this work.

## What changed in esp32_hosted

All of it is in the `update` platform.

| Change | Files |
|---|---|
| Images from a storage (`type: storage`): a single image or a manifest with versions | `update/__init__.py`, `esp32_hosted_update.cpp` |
| The image is read completely into PSRAM and its SHA256 checked before the C6 is touched; the device stops everything else first (safe mode) | `esp32_hosted_update.cpp` |
| Update through the C6's minimal image: slot query, boot the minimal image, write, boot the main image, version check | `esp32_hosted_update.cpp` |
| Serial flashing of the C6 through a UART of the P4, and reading the C6's console over the same wires | `update/c6_serial.cpp`, `update/c6_serial.h` |
| Repair over serial by itself when an update over the air fails and the manifest names a factory image | `esp32_hosted_update.cpp` |
| Switch between the stock firmware and this one, renewal of the minimal image by its revision | `esp32_hosted_update.cpp` |
| The reported version carries the firmware's build number (`3.1.0+xbt.<number>`) | `esp32_hosted_update.cpp` |
| Progress as an automation (`on_progress`) | `update/__init__.py`, `esp32_hosted_update.h` |

## Using them

These three components build on others from the same repositories: the reworked `usb_host`, the HID input
components, the storage interface, `safe_mode`. Take them from there, not from this folder alone:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome@dev
```

The configuration itself is described in [docs/esphome-config.md](../docs/esphome-config.md).

## License

The files in this folder are under the ESPHome license, [LICENSE](LICENSE): GPLv3 for the C++ runtime files, MIT for
the Python code.
