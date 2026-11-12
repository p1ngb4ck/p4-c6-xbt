# Getting started

From an ESP32-P4 board whose C6 runs the stock esp_hosted firmware to a paired Bluetooth device.

## 1. Check the board

| Check | How |
|---|---|
| The C6 has 4 MB of flash or more | The board's data sheet or schematic (module type of the C6) |
| The C6 runs esp_hosted and ESPHome reaches it | The update entity of `esp32_hosted` shows a version, Wi-Fi works |
| A storage is mounted on the P4 (SD card or USB stick), or a web server is reachable | The images come from there |

## 2. Get the firmware files

Build them ([building.md](building.md)) or take the files of a release. You need the manifest, the main image and
the minimal image; with the serial route also the factory image.

## 3. Prepare the P4

Set the P4's configuration to ESP-IDF 6.2.0 ([esphome-config.md](esphome-config.md#framework); it is not ESPHome's
default) and add the four blocks of [esphome-config.md](esphome-config.md#complete-example), with `source` pointing
at the place the manifest will be. Install that firmware on the P4. With the stock firmware
still on the C6 nothing changes yet: Wi-Fi works as before, Bluetooth is off.

## 4. Put the files in place

Copy the files into the folder `source` names, for example `/sdcard/c6/`, and name the manifest as in `source`
(`manifest.json` in the example).

## 5. Install

Press **Install** on the update entity. The device stops its apps, goes offline while the C6 is rewritten and
comes back by itself. The entity then shows `3.1.0+xbt.<number>` as the installed version. What happens in
between: [flashing.md](flashing.md#from-the-stock-firmware-to-this-one).

> [!IMPORTANT]
> Keep the device powered during this first installation. It rewrites the C6's partition table once; a power cut in
> that moment can only be repaired with a serial flash.

## 6. Pair a device

- BLE devices work at once. For Bluetooth Classic (most keyboards, gamepads, speakers) plug a USB Bluetooth dongle
  into the P4's USB host port.
- Put the device into pairing mode, search (`id(bt).scan(10)`), and connect it by its address
  (`id(bt).connect(address)` for a HID device, `id(bt).audio_connect(address)` for a speaker).
- A device that asks for a code triggers `on_code_request`; answer with `bt_offload.pair_reply`. Devices without a
  screen can use a preset code (`pin:`).
- A paired device reconnects by itself when it is switched on again.

## Later

| Task | Where |
|---|---|
| Update to a newer build | [flashing.md](flashing.md#over-the-air) |
| Go back to the stock firmware | [flashing.md](flashing.md#back-to-the-stock-firmware) |
| Something does not work | [troubleshooting.md](troubleshooting.md) |
