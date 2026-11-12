#pragma once

#include "esphome/core/defines.h"

#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD

#include <cstddef>
#include <cstdint>

namespace esphome::usb_bluetooth {

class USBBluetooth;

namespace c6_offload {

static constexpr uint8_t MESSAGE = 0x80;
static constexpr size_t FRAME_MAX = 1400;
static constexpr size_t FRAME_MAX_BIG = 1600;

using Receiver = void (*)(uint8_t type, const uint8_t *data, size_t len);

bool start(USBBluetooth *dongle);
void stop();
bool up(const uint8_t *h4, size_t len);
void want_resume();
void flush_down();
void send_available(bool available);
bool post(uint8_t type, const uint8_t *data, size_t len);
bool post_filled(uint8_t type, size_t len, size_t (*fill)(uint8_t *dst, size_t len, void *ctx), void *ctx);
void set_receiver(Receiver receiver);
void set_trace(uint8_t level);
void set_paused(bool paused);
bool audio_window();
size_t frame_max();
void set_host_ready();

}
}

#endif
