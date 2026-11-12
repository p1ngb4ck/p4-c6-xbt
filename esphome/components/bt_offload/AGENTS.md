# bt_offload: guide for AI agents

ESP32-P4 with an ESP32-C6 co-processor: Bluetooth HID input and a Bluetooth speaker run on the C6 (a custom C6
firmware); the P4 receives HID reports and sends audio over `esp32_hosted`'s HCI channel (usb_bluetooth/c6_offload:
byte 0 an H4 type, or 0x80 | message type), not over peer data (an RPC per message).

- `bt_offload.*`: `BTOffload` (a `hid::HIDInput`), the peer data handlers, connection state (`std::atomic`).
- `speaker/`: `BTOffloadSpeaker`, audio to the C6.
- One `OffloadSlot` (a `hid::HIDInput`, PSRAM) per connected device (`max_hid_devices`); the parent forwards each
  slot's keys / pointer / controls with the slot as key source, like bt_host's HID slots. Records, scan state, audio
  state (streaming, paging) come from the C6.
- Messages arrive on esp32_hosted's rx task (c6_offload::set_receiver); they are queued and handed to `hid` on the
  main loop. Audio credits do not wait for the main loop: the speaker's pump (one pumper at a time, atomics) sends PCM
  from the rx task and from play(); its loop() only reports the played frames and finishes a stop.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
