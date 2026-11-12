# ESPHome configuration on the P4

Four blocks belong to this firmware. The pins in the examples are those of the Waveshare
ESP32-P4-Module-DEV-KIT; take your board's pins from its schematic.

| Block | Job |
|---|---|
| [`esp32_hosted`](#esp32_hosted) | The SDIO link to the C6 |
| [`update: platform: esp32_hosted`](#update) | Installs and updates the C6 firmware |
| [`usb_bluetooth`](#usb_bluetooth) | The USB dongle whose controller the C6 uses for Bluetooth Classic |
| [`bt_offload`](#bt_offload) | Devices, pairing, HID input, the speaker |

All four are required; the dongle itself may be unplugged. The components come from
[p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome). See [the P4 side](how-it-works.md#the-p4-side) for the
branches that hold them.

## Framework

The P4 firmware is built on ESP-IDF 6.2.0. ESPHome's default is ESP-IDF 5.5, so the version has to be named:

```yaml
esp32:
  variant: ESP32P4
  framework:
    type: esp-idf
    version: 6.2.0
    source: https://github.com/espressif/esp-idf.git@master
```

| | |
|---|---|
| Why 6.2.0 | It is the version these components and the C6 firmware were developed, built and tested with. They have not been run on ESPHome's default version |
| Why `source` | ESP-IDF 6.2.0 has no release tag yet. `source` takes it from ESP-IDF's `master` branch |
| What else it does | The ESP-IDF copy ESPHome installs for this block is the one the C6 firmware is built with ([building.md](building.md#requirements)) |

## Complete example

```yaml
esp32_hosted:
  variant: ESP32C6
  reset_pin: GPIO54
  cmd_pin: GPIO19
  clk_pin: GPIO18
  d0_pin: GPIO14
  d1_pin: GPIO15
  d2_pin: GPIO16
  d3_pin: GPIO17

update:
  - platform: esp32_hosted
    id: c6_update
    name: "C6 Update"
    type: storage
    source: /sdcard/c6/manifest.json
    serial:
      uart_num: 2
      tx_pin: GPIO5
      rx_pin: GPIO4
      boot_pin: GPIO33
      enabled: true

usb_bluetooth:
  id: usb_bt
  c6_offload: true

bt_offload:
  id: bt
  max_hid_devices: 4
  on_status:
    - logger.log:
        format: "Bluetooth: %s"
        args: [message.c_str()]
  on_code_request:
    - logger.log:
        format: "Code wanted by %s"
        args: [device.c_str()]

speaker:
  - platform: bt_offload
    id: bt_speaker
```

## esp32_hosted

Unchanged from ESPHome: the coprocessor variant, its reset pin and the SDIO pins. Adding `bt_offload` makes the
build use SDIO packets of 2048 bytes instead of 1536, so one frame can carry a full block of pairing data.

## update

The update entity for the C6. It shows the installed and the offered version, installs on request and reports
progress.

| Option | Meaning | Default |
|---|---|---|
| `type` | Where the images come from: `storage`, `http` or `embedded` | required |
| `source` | `storage`: path of a manifest (`.json`) or of one image on a mounted storage. `http`: URL of a manifest | required for `storage` and `http` |
| `sha256` | SHA256 of the image. Required for `embedded`, and for `storage` when `source` is an image instead of a manifest | - |
| `path` / `url` | `embedded` only: the image file beside the configuration, or a URL to fetch it from at build time | one of them |
| `update_interval` | How often the manifest is checked | `6h` |
| `serial` | The serial route through the P4, see below | not set |
| `on_progress` | Automation with `x`: percent of the running step, `100` at the end, `-1` when it failed | - |
| `safe_mode_id` | The `safe_mode` component; filled in by itself when the configuration has one. The update then stops everything else on the device before it starts | automatic |

| `type` | Use it when | Switch from and to the stock firmware |
|---|---|---|
| `storage` with a manifest | the images are copied to an SD card or USB stick, or uploaded through the web file API | yes |
| `http` with a manifest | the images are on a web server | yes |
| `storage` with a single image | one known image is to be written | no |
| `embedded` | the image is compiled into the P4 firmware | no |

Only a manifest names a version and the minimal image, so only the two manifest forms can tell this firmware from
the stock firmware and switch between them.

### serial

Lets the P4 write the C6's flash over a UART, without the C6 firmware taking part. It needs the C6's UART0 and IO9
wired to three free GPIOs of the P4. How a board brings those signals out (header pins, a JST connector, pads to
solder) is in [flashing.md](flashing.md#reaching-the-c6s-uart).

| Option | Meaning | Default |
|---|---|---|
| `uart_num` | UART of the P4 used for it, 1 to 4 | required |
| `tx_pin` | P4 pin wired to the C6's RX | required |
| `rx_pin` | P4 pin wired to the C6's TX | required |
| `boot_pin` | P4 pin wired to the C6's boot pin (GPIO9) | required |
| `baud_rate` | 115200 to 2000000 | `460800` |
| `enabled` | Whether the route is used. It can also be switched at runtime (`set_serial_settings`), which is kept across restarts | `false` |

With the route enabled, a factory image as `source` is written over serial, and an update over the air that
fails is repaired over serial when the manifest names a factory image.

## usb_bluetooth

`usb_bluetooth` is our own component (USB Bluetooth dongles as HCI controllers) and runs on our reworked
`usb_host`; neither is part of upstream ESPHome. All their options:
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers/blob/main/CONFIGURATION.md),
[esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host/blob/main/CONFIGURATION.md). The ones that matter
here:

| Option | Meaning | Default |
|---|---|---|
| `c6_offload` | `true`: the dongle's HCI packets go to the C6. Cannot be combined with `bluedroid: true` | `false` |
| `vid`, `pid` | Only to pin one particular dongle to this entry; without them any Bluetooth dongle is taken by its class | `0x0000` |
| `firmware` | Firmware file the dongle needs loaded at start (for example Realtek dongles), a file beside the configuration | - |
| `firmware_config` | Configuration file belonging to `firmware` | - |

Without a dongle plugged in, the C6 runs BLE only. Plugging it in later restarts the Bluetooth host on the C6
once, with both controllers.

## bt_offload

| Option | Meaning | Default |
|---|---|---|
| `max_hid_devices` | HID devices connected at the same time, 1 to 8 | `4` |
| `pin` | PIN or passkey answered by itself when a device asks for one, up to 16 characters. For devices without a screen to enter a code on | empty |
| `c6_log_level` | Fixes the log level of the C6. Not set: the C6 follows the P4's log level and stays silent below `VERY_VERBOSE` | not set |
| `on_status` | Automation with `message`: text of a pairing or connection event, for a status line | - |
| `on_code_request` | Automation with `device`, `pin` (`true`: a PIN, `false`: a passkey) and `preset`: a device asks for a code. Answer with `bt_offload.pair_reply` or `bt_offload.pair_cancel` | - |
| `on_connected` | Automation with `device`: a device connected | - |
| `on_manufacturer_data` | Automation with `x` (bytes), `id` and `device`: BLE advertisement manufacturer data | - |
| `on_service_data` | Automation with `x` (bytes), `uuid` and `device`: BLE advertisement service data | - |

It also takes the options of ESPHome's HID input (for example `layout` for the keyboard layout) for the devices it
connects.

### Actions

| Action | Meaning |
|---|---|
| `bt_offload.pair_reply` with `code` | Sends the code the user entered for the running pairing |
| `bt_offload.pair_cancel` | Ends the running pairing |

### From lambdas

| Call | Meaning |
|---|---|
| `id(bt).scan(seconds)` / `cancel_scan()` / `is_scanning()` | Search for devices |
| `id(bt).connect(address)` / `disconnect(address)` / `is_connected(address)` | Pair with or connect a found HID device, by its address as text |
| `id(bt).audio_connect(address)` / `audio_disconnect()` / `is_connected()` | The one audio device |
| `id(bt).forget(address)` | Removes a device and its bond |
| `id(bt).set_audio_volume(volume)` | Volume of the audio device |
| `id(bt).request_status()` | Asks the C6 for its status values now |

### Platforms

| Platform | What it adds |
|---|---|
| `speaker: platform: bt_offload` | The paired audio device as an ESPHome speaker: 16 bit, stereo, 44100 Hz. One per `bt_offload` |
| `number: platform: bt_offload` | Audio delay in milliseconds, -800 to 1600 in steps of 10, to line sound up with a picture |
| `text: platform: bt_offload` with `type: preset` or `type: code` | The preset PIN and the code entry as entities, for devices without a screen |
| `sensor: platform: bt_offload` | Status values of the C6, each optional, `update_interval` 60 s by default: `uptime`, `internal_free`, `internal_min_free`, `internal_largest_block`, `tasks`, `worker_stack_free`, `audio_ring_free`, `a2dp_calls`, `a2dp_max`, `command_max`, `frames_in`, `frames_out`, `send_max`, `a2dp_underruns`, `reset_reason`, `waiting_messages`, `waiting_h4`, `crashed_starts` |

What the values mean: [troubleshooting.md](troubleshooting.md#status-values).

## Changing the update source at runtime

The source can be changed without a new build, for example to offer the stock firmware once:

```yaml
api:
  actions:
    - action: c6_update_source
      variables:
        path: string
      then:
        - lambda: id(c6_update).set_storage_path(path);
```
