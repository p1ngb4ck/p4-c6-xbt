# Flashing the C6

There are three ways to write the C6, and one way to move between the stock firmware and this one. The two serial
ways share one requirement, [access to the C6's UART](#reaching-the-c6s-uart).

| Way | Needs | Writes | Settings and bonds on the C6 | Wi-Fi during it | Use it for |
|---|---|---|---|---|---|
| [Over the air](#over-the-air) | The update entity and the images on a storage or web server | Main image, when needed the minimal image | kept | away while the C6 restarts | Every normal update |
| [Serial through the P4](#serial-through-the-p4) | [Access to the C6's UART](#reaching-the-c6s-uart), wired to three GPIOs of the P4; this work's firmware on the P4 with the `serial` block | The whole flash (factory image) | erased, bonds restored from the P4's copy | away until it is done | First installation without the stock firmware's help, repair, without any extra hardware |
| [USB adapter on the C6](#usb-adapter-on-the-c6) | [Access to the C6's UART](#reaching-the-c6s-uart), a USB-UART adapter, `esptool` on a PC | The whole flash (factory image) | erased, bonds restored from the P4's copy | away until the C6 restarts | Flashing whatever firmware the P4 runs |
| [From the stock firmware and back](#from-the-stock-firmware-and-back) | As over the air | Images and the partition table | kept | away while the C6 restarts | Trying the firmware, going back |

Which firmware a C6 runs shows in the update entity's installed version: `3.1.0` is the stock firmware,
`3.1.0+xbt.<number>` this one.

## The files

Copy these files of a build ([building.md](building.md#what-a-build-produces)) into one folder on a storage of the
P4, for example `/sdcard/c6/`, or onto a web server:

| File | Needed for |
|---|---|
| `manifest_bt-offload.json` | always; this is the update's `source` |
| `network_adapter_esp32c6_bt-offload.bin` | always |
| `network_adapter_esp32c6_minimal.bin` | the switch from the stock firmware, a renewal of the minimal image |
| `network_adapter_esp32c6_bt-offload.factory.bin` | serial flashing, and the repair after a failed update when the manifest names it |

On a web server the manifest's `url` fields have to be full URLs. On a storage, names without a folder are looked
up beside the manifest.

## Over the air

1. Put the files in place and make sure the update's `source` points at the manifest.
2. Press **Install** on the update entity (Home Assistant, the web server or the device's own UI).

What the device does:

| Step | Visible as |
|---|---|
| Reads the manifest, compares versions | The entity shows the offered version |
| Stops everything else on the device (safe mode) | Apps close, playback stops |
| Reads the image into memory, checks its SHA256 | `SHA256 verified successfully` |
| Tells the user when Wi-Fi is the only network | `Wi-Fi is the only network: offline during the C6 update, back by itself` |
| Restarts the C6 into its minimal image, writes the main image | `C6 update 5 %` ... `100 %` |
| Restarts the C6 into the new main image, compares the version | `C6 updated, version 3.1.0+xbt.<number>` |
| Brings Wi-Fi back and returns to normal operation | The device is reachable again |

Things to know:

- The P4 does not restart. A device whose only network is the C6's Wi-Fi is unreachable until the last step.
- The offered version must not be newer than the esp_hosted version of the P4's own firmware. Update the P4 first.
- The same version is not installed twice. Raise the build number to install a rebuild.
- Bluetooth devices reconnect by themselves afterwards; their bonds are on the C6 and stay.
- The update needs free PSRAM on the P4 for the image (about 2.2 MB, with a switch or a minimal renewal about
  3.3 MB). That is why everything else is stopped first.
- An image the C6 rejects leaves it in the minimal image with Wi-Fi working. Install again, or let the serial
  route repair it.

## Reaching the C6's UART

Both serial ways write the C6 through its ROM bootloader. For that they need the same three signals of the C6:
UART0 TX, UART0 RX and IO9 (the boot pin). Getting at these three signals is the real hurdle of serial flashing,
and it is the same hurdle for both ways. How a board brings them out differs:

| The C6's UART0 and IO9 are | Example board | What you need |
|---|---|---|
| normal pins on the 40-pin header | Guition JC-ESP32P4-M3-Dev | Jumper wires |
| on a JST connector of their own | Waveshare ESP32-P4-Module-DEV-KIT | A JST-to-jumper-header cable |
| on pads for a normal pin header that is not fitted | Waveshare ESP32-P4-WIFI6-POE-ETH | Solder a pin header |
| on pads for a JST connector that is not fitted | Waveshare ESP32-P4-WIFI6-DEV-KIT (the older board in Raspberry Pi form factor) | Solder the JST connector, then the cable as above |

The board's schematic shows which case applies. From there the two ways differ only in where the three signals go:

| | [Serial through the P4](#serial-through-the-p4) | [USB adapter on the C6](#usb-adapter-on-the-c6) |
|---|---|---|
| The three signals go to | three free GPIOs of the P4 on its 40-pin header | a 3.3 V USB-UART adapter on a PC |
| Extra hardware | none | the adapter |
| Firmware on the P4 | this work's ESPHome firmware: the mechanism is part of our `esp32_hosted` update platform | no particular one; it works whatever firmware the P4 runs |
| Started | from the update entity, also by itself after a failed update over the air | with `esptool` on the PC |

Through the P4 is the convenient way once the wires are plugged: no adapter, no PC, and the device can repair its
C6 by itself. The adapter is the way that depends on nothing.

## Serial through the P4

The P4 puts the C6 into its ROM bootloader and writes the flash itself. The C6's own firmware takes no part, so
this works on an empty or broken C6. It needs this work's firmware on the P4.

### Wiring

| P4 | C6 | Option |
|---|---|---|
| a free GPIO, as UART TX | RX of UART0 | `tx_pin` |
| a free GPIO, as UART RX | TX of UART0 | `rx_pin` |
| a free GPIO | IO9 (boot) | `boot_pin` |
| the reset pin of `esp32_hosted` | EN (reset) | already wired on every board |

The first three are the signals of [Reaching the C6's UART](#reaching-the-c6s-uart), plugged onto free GPIOs of the
P4's 40-pin header. Ground is shared on the board.

### Steps

1. Add the [`serial`](esphome-config.md#serial) block with `enabled: true` and install that P4 firmware.
2. Put the factory image and a manifest on the storage whose `url` and `sha256` are those of the factory image:

   ```json
   {"versions": [{"version": "3.1.0+xbt.1",
                  "url": "network_adapter_esp32c6_bt-offload.factory.bin",
                  "sha256": "..."}]}
   ```

3. Point `source` at that manifest and press **Install**. The update sees a factory image and takes the serial
   route.

Things to know:

- The whole flash is written, about 3.9 MB at 460800 baud. Progress shows in steps of 5 %.
- The C6's settings are erased with it. The P4 writes the Bluetooth bonds back from its copy
  (`/sdcard/bluetooth/c6_bonds.bin`) when the C6 is up again.
- With the route enabled and `factory_url` / `factory_sha256` in the normal manifest, the P4 takes this route by
  itself when an update over the air has failed.

## USB adapter on the C6

This way needs nothing particular on the P4: no firmware of ours, no free GPIOs. Connect a 3.3 V USB-UART adapter to the
C6's UART0 and ground ([Reaching the C6's UART](#reaching-the-c6s-uart)), hold the C6's boot pin (IO9) low while
it is reset, then:

```bash
esptool.py --chip esp32c6 --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 network_adapter_esp32c6_bt-offload.factory.bin
```

Reset the board afterwards. The P4 has to run a firmware with the blocks of
[esphome-config.md](esphome-config.md), otherwise Bluetooth stays off (Wi-Fi works either way).

## From the stock firmware and back

Both directions are started like an update over the air. The difference is only which manifest is offered.

### Requirements

| Requirement | Why |
|---|---|
| C6 flash of 4 MB or more | This firmware's flash layout needs it; a smaller flash is refused before anything is written |
| The P4 firmware contains the blocks of [esphome-config.md](esphome-config.md) and is at least as new (esp_hosted version) as both C6 firmwares | The P4 drives every step |
| The manifest names the minimal image | It is the first thing written onto a stock C6 |
| For the way back: a stock image of at most 1.75 MB | It has to fit the stock slot |
| Stable power | One step rewrites the partition table; a power cut in that moment needs a serial flash to recover |

### From the stock firmware to this one

Offer this firmware's manifest ([the files](#the-files)) and press **Install**.

| Step | What happens on the C6 |
|---|---|
| 1 | The stock firmware takes the minimal image like a normal update and starts it |
| 2 | The minimal image copies itself into the first app slot, if it is not already running there |
| 3 | It writes this firmware's partition table. The first slot starts at the same address in both tables, so the C6 still boots |
| 4 | Restart. The minimal image copies itself into its own slot and starts from there |
| 5 | The main image is written into the first slot and started |

### Back to the stock firmware

Offer a manifest of the stock firmware and press **Install**. With `type: http` this can be ESPHome's own
manifest, `https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json`. On a storage, write one:

```json
{"versions": [{"version": "3.1.0",
               "url": "network_adapter_esp32c6.bin",
               "sha256": "..."}]}
```

| Step | What happens on the C6 |
|---|---|
| 1 | The main image hands over to the minimal image |
| 2 | The stock image is written into the first slot |
| 3 | The minimal image writes the stock partition table |
| 4 | Restart. The stock firmware runs |

After this the C6 is a stock C6 again: BLE through the P4's own Bluetooth components, no Bluetooth Classic. The
bonds made with this firmware stay in the C6's settings and in the P4's copy and are used again after the next
switch to it.

A version without `+xbt.` in a manifest means stock, one with it means this firmware. An image given directly
(`type: embedded`, or `storage` with an image as `source`) never switches.
