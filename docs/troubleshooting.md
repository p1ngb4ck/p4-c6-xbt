# Troubleshooting

## Messages of the update

They appear in the P4's log with the tag `esp32_hosted.update`; the last one is also the update entity's state.

| Message | Meaning | What to do |
|---|---|---|
| `No manifest at <path>` | The manifest could not be read: wrong path, storage not mounted, or the storage is in use | Check the path and the storage, install again |
| `No compatible firmware version found (host is <version>)` | Every version in the manifest is newer than the P4's esp_hosted | Update the P4's firmware first |
| `C6 is up to date` | Installed and offered version are equal | Nothing. To install a rebuild, give it a higher build number |
| `SHA256 mismatch` | The image does not match the manifest | Copy the files again |
| `No app image: the coprocessor is not touched` | The file is no ESP32 application image | Check which file the manifest names |
| `No minimal image in the manifest: the stock C6 firmware stays` | Switching from the stock firmware needs `minimal_url` and `minimal_sha256` | Use the manifest of a current build with its minimal image |
| `The C6 has no minimal firmware: no update over esp_hosted` | The second slot of the C6 is empty | Write the factory image over serial |
| `C6 is in its minimal firmware: back to main` | At start the C6 was found in its minimal image with a valid main image | Nothing, it is switched back |
| `C6 is in its minimal firmware and has no main firmware: update it` | An update stopped half way | Install again |
| `Stock firmware not written: image too large for the stock slot` | The stock image is larger than 1.75 MB | Use another stock image |
| `Stock firmware not written: the C6 minimal image is too old` | The minimal image on the C6 cannot write a partition table | Install a current build of this firmware first; it renews the minimal image |
| `C6 slot command <n> failed: 0x<code>` | The C6 refused a step of the switch; the code is an ESP-IDF error | See the table below |
| `C6 update over esp_hosted failed: factory image over serial` | The update over the air failed and the serial route takes over | Wait for it to finish |
| `C6 runs <a> after the update to <b>` | The C6 came back with another version than installed | The new image did not start; install again or flash over serial |
| `The C6 did not come back: <n>` | No link to the C6 after its restart | Power cycle the board; if the C6 stays away, flash it over serial or with a USB adapter |

| Slot command | Step | Typical error |
|---|---|---|
| 4 | The minimal image copies itself into the other slot | `0x104` the image does not fit the slot |
| 5 | The minimal image writes a partition table | `0x104` flash smaller than 4 MB or stock image too large, `0x105` no application in the first slot |

## Status values

The `sensor` platform of `bt_offload` publishes them; `id(bt).request_status()` asks for them at once.

| Value | Meaning | Worth a look when |
|---|---|---|
| `internal_free` | Free RAM of the C6 now | - |
| `internal_min_free` | Lowest free RAM since the C6 started. The dips come while devices connect | it falls below about 4 KB |
| `internal_largest_block` | Largest single free block | it is far below `internal_free` for long |
| `crashed_starts` | Starts after a crash in a row. At 3 the C6 starts without Bluetooth; two minutes of running clear it | it is not 0 |
| `reset_reason` | ESP-IDF reset reason of the C6's last start (1 power on, 3 software, 4 panic, 5 to 7 a watchdog) | it is 4 to 7 |
| `waiting_messages`, `waiting_h4` | Frames waiting to be sent to the P4 or to the dongle | they stay above 0 |
| `audio_ring_free` | Free bytes of the 16384 byte audio buffer | - |
| `a2dp_underruns` | Audio callbacks that found too little data. Also counts while nothing plays, so it is no measure of stutter | - |
| `a2dp_calls`, `a2dp_max` | Audio callbacks since the last status and the longest one | - |
| `frames_in`, `frames_out` | Frames from and to the P4 since the last status | - |
| `send_max`, `command_max` | Longest send to the P4 and longest command | they reach seconds |
| `worker_stack_free` | Unused stack of the command task | it falls below about 500 bytes |
| `tasks`, `uptime` | Tasks on the C6, time since its start | - |

## The C6 crashed

After a crash the C6 reports it at its next start, in the P4's log:

```text
C6 log: restarted after a task watchdog, crashed starts 1
C6 log: crash before this start: task hciT pc 0x4219999e ra 0x4207f634 sp 0x4085fc90 mcause 0x0 mtval 0xffffffff elf 00563b8f
C6 log: crash stack: 4207f634 4207d70c 4205b21a ...
```

The addresses belong to the build named by `elf` (the first 8 digits of the SHA256 of that build's ELF file).
With the ELF file of the build:

```bash
riscv32-esp-elf-addr2line -pfiaC -e elf/xbt.<number>.elf 0x4219999e 0x4207f634 0x4207d70c
```

## Bluetooth

| Symptom | Likely cause | What to do |
|---|---|---|
| No Classic device is found, BLE works | No dongle, or the dongle needs a firmware file | Plug it in; set `firmware` for dongles that need one |
| A speaker refuses to pair | It is held by another source it is bonded to | Switch the other source off while pairing |
| A device does not reconnect after a serial flash | The bonds were erased with the flash and no copy was on the P4's storage | Pair again |
| Bluetooth is off, Wi-Fi works | The C6 runs the stock firmware or its minimal image, or it started in safe mode after crashes | Check the installed version and `crashed_starts` |

## The C6 does not answer at all

The P4's log then shows SDIO command timeouts or `SDIO aggr: no slave credits`. Power cycle the board. If it
happens again with the same steps, note what was connected and what was done just before; the crash record of the
next start names the task.
