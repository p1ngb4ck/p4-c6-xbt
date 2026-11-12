#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_OFFLOAD_SPEAKER

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../bt_offload.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>

namespace esphome::bt_offload {

class BTOffloadSpeaker : public speaker::Speaker, public Component, public Parented<BTOffload> {
 public:
  static constexpr size_t RING_BYTES = 52920;
  static constexpr size_t CHUNK = 1024;
  static constexpr uint32_t C6_RING = 16384;
  static constexpr uint32_t BYTES_PER_FRAME = 4;

  void setup() override;
  void loop() override;
  void dump_config() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  void on_audio_state(bool connected);
  void on_credit(uint32_t bytes);
  void on_window(uint32_t limit, uint32_t waiting);

 protected:
  static size_t fill_(uint8_t *dst, size_t len, void *ctx);
  static size_t fill_at_(uint8_t *dst, size_t len, void *ctx);
  uint32_t room_() const;
  size_t write_(const uint8_t *data, size_t length);
  void release_();
  void apply_volume_();
  void pump_();
  bool send_();

  mutable Mutex lock_;
  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  SemaphoreHandle_t space_{nullptr};
  StaticSemaphore_t space_control_;
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> credit_{0};
  std::atomic<bool> window_{false};
  std::atomic<uint32_t> sent_{0};
  std::atomic<uint32_t> limit_{0};
  size_t filled_{0};
  std::atomic<bool> pumping_{false};
  std::atomic<bool> again_{false};
  std::atomic<uint32_t> credited_frames_{0};
  std::atomic<int64_t> credited_at_{0};
};

}

#endif
