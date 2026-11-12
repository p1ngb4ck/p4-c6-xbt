# Measurements

Everything here was measured on one device: a Waveshare ESP32-P4-Module-DEV-KIT (ESP32-P4 rev 3.2, ESP32-C6 with
4 MB flash on SDIO), a Realtek RTL8851BU USB Bluetooth dongle, ESP-IDF 6.2.0, esp_hosted 3.1.0. RAM values are the
C6's internal RAM in bytes; the C6 has no PSRAM. "Lowest" is the lowest free value since the C6 started.

## Image sizes (build 139)

| Image | Size | Slot | Used |
|---|---|---|---|
| Main image | 2,176,416 | `ota_0`, 2,686,976 | 81 % |
| Minimal image | 1,069,616 | `ota_1`, 1,376,256 | 78 % |
| Factory image (bootloader, table, both images) | 3,887,664 | whole flash, 4,194,304 | 93 % |
| Stock esp_hosted 3.1.0 image, for comparison | 1,385,536 | stock slot, 1,835,008 | 76 % |

## Static RAM of the C6

RAM taken by code placed in RAM, initialised data and zeroed data, from the build's size report. The chip has
452,112 bytes.

| Image | Static RAM | Share | Left for the heap |
|---|---|---|---|
| Main image | 174,504 | 38.6 % | 277,608 |
| Minimal image | 133,936 | 29.6 % | 318,176 |

## Free RAM of the C6 in use

| State | Free | Largest block | Lowest |
|---|---|---|---|
| Started, Bluetooth host up, no device connected | 93,668 | 77,824 | 77,000 |
| Bluetooth receiver connected (A2DP), nothing playing | about 9,100 less than without it (measured on build 129: 87,908 to 78,800) | - | - |
| Two BLE HID devices, two Classic HID devices, receiver, music playing | 54,416 | 35,840 | 6,264 |

The lowest value is reached for moments while several devices connect at the same time (service discovery,
pairing). In steady state the RAM comes back in full.

### Cost per connected device

| Device | RAM while connected |
|---|---|
| BLE mouse | about 12,400 |
| BLE remote control | about 12,800 |
| Classic keyboard | about 7,400 |
| Classic gamepad | about 8,000 (estimated from the keyboard) |
| Audio link to a receiver (A2DP) | about 9,100, plus the 16,384 byte audio buffer that is reserved at start |

These values are from an earlier build; the last three RAM changes of the table below lowered the cost per open
connection by about 1 KB per channel.

## What the RAM work brought

How the free RAM with all five devices and music moved over the builds of the development:

| Build | Free with all devices and music | Lowest | Audio |
|---|---|---|---|
| 129 | 16,600 to 21,000 | 1,480 to 4,700 | short stutter when a BLE device reconnected |
| 135 | 18,200 to 25,600 | 3,088 | stutter after pairing all devices |
| 137 | 54,416 | 6,264 | no stutter |

| Change | RAM gained on the C6 |
|---|---|
| esp_hosted's SDIO buffers sized to the packets instead of the hardware maximum | 48,700 |
| Flash driver from the chip's ROM instead of a copy in RAM | 11,548 |
| Wi-Fi sleep code no longer placed in RAM (the C6 never sleeps here) | 3,808 |
| Outgoing frames handed to esp_hosted where they lie, no own batch buffer | 1,600 and one copy less per frame |
| Receive ring sized to its real need | 1,360 |
| Own task stacks sized from their measured peaks | 3,584 |
| Three L2CAP queues per channel created at first use | 1,068 per open channel |
| Audio packets sized to the link's packet size | up to 3 KB per queued packet |
| Trace ring only while a trace is switched on | 6,144 |

Not touched on purpose: the Wi-Fi buffers and block-ack windows of esp_hosted's own configuration, and the parts of
the Bluetooth host later features need.

## Task stacks on the C6

Free stack with all five devices connected and music playing.

| Task | Stack | Free |
|---|---|---|
| Command worker | 3,072 | 1,484 |
| Sender to the P4 | 1,536 | 1,008 |
| Receiver from the P4 | 2,048 | 992 |
| BLE connect task | 3,072 | 1,128 |
| Bluetooth host, upper layer (BTU) | 8,192 | 5,260 |
| Bluetooth host, HCI task | library default | 1,880 |
| esp_hosted receive task | 4,096 | 3,516 |

## Update times

Time from the notice on the device ("offline during the C6 update") to the new firmware answering. The P4 keeps
running in every case.

| Way | What is written | Time | C6 restarts | C6 settings and bonds |
|---|---|---|---|---|
| Update over the air | Main image, 2.2 MB in 16 s | 24 to 25 s | 2 | kept |
| Update over the air with a renewal of the minimal image | Minimal image, 1.1 MB in 8 s, then the main image, 2.2 MB in 16 s | 40 s | 4 | kept |
| Back to the stock firmware | Stock image, 1.4 MB in 10 s, then the stock partition table | 18 s | 2 | kept |
| From the stock firmware | Minimal image through the stock firmware's own update, 1.1 MB in 5 s, our partition table, then the main image, 2.2 MB in 16 s | 41 s | 5 | kept |
| Serial through the P4 at 460800 baud | Factory image, 3.9 MB in 102 s | about 2.5 min | 1 | erased, bonds restored from the P4's copy |

The three switch and renewal runs were done one after the other on the same device without restarting the P4:
its uptime ran on, its packages kept running, and after each run Wi-Fi was back by itself. With the stock firmware
on the C6 the P4 was reachable over Wi-Fi as before.

## The P4 during an update

| | |
|---|---|
| PSRAM taken for the image | the image size: about 2.2 MB, with a switch or a renewal of the minimal image about 3.3 MB |
| Internal RAM taken | none beyond the update task's stack (6,144 bytes) |
| Returned afterwards | all of it |
