#include "bt_offload_speaker.h"

#ifdef USE_BT_OFFLOAD_SPEAKER

#include "esphome/components/usb_bluetooth/c6_offload.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome::bt_offload {

static const char *const TAG = "bt_offload.speaker";

static constexpr uint8_t MSG_AUDIO_PCM = 0x32;
static constexpr uint8_t MSG_AUDIO_PCM_AT = 0x38;

void BTOffloadSpeaker::setup() {
  this->space_ = xSemaphoreCreateBinaryStatic(&this->space_control_);
  this->parent_->set_speaker(this);
  this->disable_loop();
}

void BTOffloadSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "Bluetooth offload speaker: 16 bit stereo, 44100 Hz"); }

size_t BTOffloadSpeaker::write_(const uint8_t *data, size_t length) {
  LockGuard guard(this->lock_);
  if (this->ring_ == nullptr)
    return 0;
  return this->ring_->write_without_replacement(data, length, 0, true);
}

size_t BTOffloadSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING)
    this->start();
  if (this->state_ != speaker::STATE_RUNNING)
    return 0;
  if (!this->connected_.load())
    return length;
  size_t written = this->write_(data, length);
  if (written == 0 && ticks_to_wait > 0 && xSemaphoreTake(this->space_, ticks_to_wait) == pdTRUE)
    written = this->write_(data, length);
  if (written > 0)
    this->pump_();
  return written;
}

void BTOffloadSpeaker::start() {
  LockGuard guard(this->lock_);
  if (this->ring_ == nullptr) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < RING_BYTES) {
      ESP_LOGE(TAG, "No PSRAM block of %u bytes for the PCM ring", static_cast<unsigned>(RING_BYTES));
      return;
    }
    this->ring_ = ring_buffer::RingBuffer::create(RING_BYTES);
    if (this->ring_ == nullptr) {
      ESP_LOGE(TAG, "PCM ring of %u bytes not created", static_cast<unsigned>(RING_BYTES));
      return;
    }
  }
  this->state_ = speaker::STATE_RUNNING;
}

void BTOffloadSpeaker::stop() {
  this->state_ = speaker::STATE_STOPPED;
  this->enable_loop_soon_any_context();
}

void BTOffloadSpeaker::finish() {
  if (this->state_ == speaker::STATE_RUNNING) {
    this->state_ = speaker::STATE_STOPPING;
    this->pump_();
    this->enable_loop_soon_any_context();
  }
}

bool BTOffloadSpeaker::has_buffered_data() const {
  LockGuard guard(this->lock_);
  return this->ring_ != nullptr && this->ring_->available() > 0;
}

void BTOffloadSpeaker::release_() {
  LockGuard guard(this->lock_);
  if (this->state_ == speaker::STATE_STOPPED)
    this->ring_.reset();
}

size_t BTOffloadSpeaker::fill_(uint8_t *dst, size_t len, void *ctx) {
  auto *self = static_cast<BTOffloadSpeaker *>(ctx);
  return self->ring_->read(dst, len, 0);
}

size_t BTOffloadSpeaker::fill_at_(uint8_t *dst, size_t len, void *ctx) {
  auto *self = static_cast<BTOffloadSpeaker *>(ctx);
  const uint32_t position = self->sent_.load(std::memory_order_acquire);
  std::memcpy(dst, &position, sizeof(position));
  self->filled_ = self->ring_->read(dst + sizeof(position), len - sizeof(position), 0);
  return self->filled_ > 0 ? self->filled_ + sizeof(position) : 0;
}

uint32_t BTOffloadSpeaker::room_() const {
  if (!this->window_.load(std::memory_order_acquire))
    return this->credit_.load(std::memory_order_acquire);
  const int32_t room = static_cast<int32_t>(this->limit_.load(std::memory_order_acquire) -
                                            this->sent_.load(std::memory_order_acquire));
  return room > 0 ? static_cast<uint32_t>(room) : 0;
}

bool BTOffloadSpeaker::send_() {
  bool sent = false;
  while (this->connected_.load(std::memory_order_acquire)) {
    const uint32_t credit = this->room_();
    if (credit == 0)
      break;
    LockGuard guard(this->lock_);
    if (this->ring_ == nullptr)
      break;
    const size_t available = this->ring_->available();
    const bool last = this->state_ == speaker::STATE_STOPPING;
    size_t len = std::min<size_t>(std::min<size_t>(available, CHUNK), credit);
    len -= len % BYTES_PER_FRAME;
    if (len == 0 || (len < CHUNK && !last))
      break;
    if (this->window_.load(std::memory_order_acquire)) {
      if (!usb_bluetooth::c6_offload::post_filled(MSG_AUDIO_PCM_AT, len + sizeof(uint32_t), &BTOffloadSpeaker::fill_at_,
                                                  this))
        break;
      this->sent_.fetch_add(static_cast<uint32_t>(this->filled_), std::memory_order_acq_rel);
    } else {
      if (!usb_bluetooth::c6_offload::post_filled(MSG_AUDIO_PCM, len, &BTOffloadSpeaker::fill_, this))
        break;
      this->credit_.fetch_sub(static_cast<uint32_t>(len), std::memory_order_acq_rel);
    }
    sent = true;
  }
  return sent;
}

void BTOffloadSpeaker::pump_() {
  this->again_.store(true, std::memory_order_release);
  while (!this->pumping_.exchange(true, std::memory_order_acq_rel)) {
    bool sent = false;
    while (this->again_.exchange(false, std::memory_order_acq_rel))
      sent |= this->send_();
    this->pumping_.store(false, std::memory_order_release);
    if (sent)
      xSemaphoreGive(this->space_);
    if (!this->again_.load(std::memory_order_acquire))
      break;
  }
}

void BTOffloadSpeaker::loop() {
  const uint32_t frames = this->credited_frames_.exchange(0, std::memory_order_acq_rel);
  if (frames > 0)
    this->audio_output_callback_.call(frames, this->credited_at_.load(std::memory_order_acquire));
  if (this->state_ == speaker::STATE_STOPPED) {
    this->release_();
    this->disable_loop();
    return;
  }
  if (this->state_ == speaker::STATE_STOPPING && (!this->connected_.load() || !this->has_buffered_data())) {
    this->state_ = speaker::STATE_STOPPED;
    this->release_();
  }
  this->disable_loop();
}

void BTOffloadSpeaker::on_audio_state(bool connected) {
  this->connected_.store(connected);
  this->credit_.store(connected ? C6_RING : 0, std::memory_order_release);
  this->limit_.store(this->sent_.load(std::memory_order_acquire) + (connected ? C6_RING : 0),
                     std::memory_order_release);
  this->window_.store(connected && usb_bluetooth::c6_offload::audio_window(), std::memory_order_release);
  {
    LockGuard guard(this->lock_);
    if (this->ring_ != nullptr)
      this->ring_->reset();
  }
  if (connected)
    this->apply_volume_();
  this->pump_();
  this->enable_loop();
}

void BTOffloadSpeaker::on_credit(uint32_t bytes) {
  uint32_t credit = this->credit_.load(std::memory_order_acquire);
  while (!this->credit_.compare_exchange_weak(credit, std::min<uint32_t>(credit + bytes, C6_RING),
                                              std::memory_order_acq_rel))
    ;
  this->credited_at_.store(esp_timer_get_time(), std::memory_order_release);
  this->credited_frames_.fetch_add(bytes / BYTES_PER_FRAME, std::memory_order_acq_rel);
  this->pump_();
  this->enable_loop_soon_any_context();
}

void BTOffloadSpeaker::on_window(uint32_t limit, uint32_t waiting) {
  const uint32_t before = this->limit_.exchange(limit, std::memory_order_acq_rel);
  const int32_t moved = static_cast<int32_t>(limit - before);
  if (moved > 0) {
    this->credited_at_.store(esp_timer_get_time(), std::memory_order_release);
    this->credited_frames_.fetch_add(static_cast<uint32_t>(moved) / BYTES_PER_FRAME, std::memory_order_acq_rel);
  }
  if (waiting == 0 && this->room_() < CHUNK && this->connected_.load(std::memory_order_acquire)) {
    const uint32_t position = this->sent_.load(std::memory_order_acquire);
    usb_bluetooth::c6_offload::post(MSG_AUDIO_PCM_AT, reinterpret_cast<const uint8_t *>(&position), sizeof(position));
  }
  this->pump_();
  this->enable_loop_soon_any_context();
}

void BTOffloadSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  this->apply_volume_();
}

void BTOffloadSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  this->apply_volume_();
}

void BTOffloadSpeaker::apply_volume_() {
  if (!this->connected_.load())
    return;
  const float volume = this->is_silent_() ? 0.0f : std::min(this->volume_, 1.0f);
  this->parent_->set_audio_volume(static_cast<uint8_t>(std::lround(volume * 127.0f)));
}

}

#endif
