#include "c6_offload.h"

#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD

#include "usb_bluetooth.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/static_task.h"
#include "eh_host_feat_bt_mcu.h"
#include "esp_heap_caps.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::usb_bluetooth::c6_offload {

static const char *const TAG = "usb_bt_c6";

static constexpr uint8_t TYPE_START = 0x01;
static constexpr uint8_t TYPE_FLOW = 0x04;
static constexpr uint8_t TYPE_HELLO = 0x05;
static constexpr uint8_t TYPE_BATCH_OK = 0x06;
static constexpr uint8_t TYPE_HOST_READY = 0x07;
static constexpr uint8_t TYPE_RX_CREDIT = 0x08;
static constexpr uint8_t TYPE_SYNC = 0x09;
static constexpr uint8_t TYPE_SYNC_ACK = 0x0A;
static constexpr uint8_t TYPE_AUDIO_PCM = 0x32;
static constexpr uint8_t TYPE_AUDIO_PCM_AT = 0x38;
static constexpr uint8_t HELLO_BATCH = 0x01;
static constexpr uint8_t HELLO_CREDIT_SYNC = 0x02;
static constexpr uint8_t HELLO_AUDIO_WINDOW = 0x04;
static constexpr uint8_t HELLO_BIG_FRAMES = 0x08;
static constexpr uint8_t HOST_CREDIT_TOTAL = 0x02;
#if defined(ESP_TRANSPORT_SDIO_MAX_BUF_SIZE) && ESP_TRANSPORT_SDIO_MAX_BUF_SIZE >= 2048
static constexpr uint8_t HOST_BIG_FRAMES = 0x08;
#else
static constexpr uint8_t HOST_BIG_FRAMES = 0;
#endif
static constexpr uint32_t CREDIT_SYNC_MS = 1000;
static constexpr uint8_t FRAME_BATCH = 0x7F;
static constexpr size_t BATCH_MAX = 1500;

static constexpr size_t START_NAME_LEN = 32;
static constexpr size_t QUEUE_DEPTH = 32;
static constexpr size_t DOWN_DEPTH = 32;
static constexpr size_t AUDIO_DEPTH = 16;
static constexpr uint32_t SENDER_STACK = 3072;
static constexpr UBaseType_t SENDER_PRIORITY = 5;

struct Packet {
  uint16_t len;
  uint8_t data[];
};

static USBBluetooth *s_dongle = nullptr;
static eh_host_bt_mcu_hci_tx_fn_t s_tx = nullptr;
static std::atomic<Receiver> s_receiver{nullptr};
static QueueHandle_t s_queue = nullptr;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[QUEUE_DEPTH * sizeof(Packet *)];
static QueueHandle_t s_down = nullptr;
static StaticQueue_t s_down_control;
static uint8_t s_down_storage[DOWN_DEPTH * sizeof(Packet *)];
static SemaphoreHandle_t s_down_lock = nullptr;
static StaticSemaphore_t s_down_lock_control;
static std::atomic<bool> s_flow_off{false};
static std::atomic<bool> s_resume{false};
static std::atomic<uint8_t> s_trace{0};
static std::atomic<bool> s_paused{false};
static std::atomic<bool> s_peer_batch{false};
static std::atomic<bool> s_host_ready{false};
static std::atomic<bool> s_credit_on{false};
static std::atomic<bool> s_credit_sync{false};
static std::atomic<uint32_t> s_grant{0};
static std::atomic<uint32_t> s_sent{0};
static std::atomic<uint32_t> s_done{0};
static std::atomic<uint32_t> s_lost{0};
static std::atomic<uint32_t> s_sync_sent{0};
static std::atomic<uint8_t> s_sync_id{0};
static std::atomic<bool> s_audio_window{false};
static std::atomic<bool> s_big_frames{false};
static QueueHandle_t s_audio = nullptr;
static StaticQueue_t s_audio_control;
static uint8_t s_audio_storage[AUDIO_DEPTH * sizeof(Packet *)];
static StaticTask s_sender;

static constexpr uint8_t H4_COMMAND = 0x01;
static constexpr uint8_t H4_ACL = 0x02;
static constexpr uint8_t H4_EVENT = 0x04;
static constexpr uint16_t OP_HOST_COMPLETED = 0x0C35;
static constexpr uint8_t EVT_COMPLETED_PACKETS = 0x13;
static constexpr uint8_t EVT_LE_META = 0x3E;
static constexpr uint8_t LE_ADVERTISING_REPORT = 0x02;
static constexpr uint16_t L2CAP_SIGNALLING = 0x0001;
static constexpr size_t TRACE_BYTES = 24;

static void trace_(const char *direction, const uint8_t *h4, size_t len) {
  const uint8_t level = s_trace.load(std::memory_order_relaxed);
  if (level == 0 || len < 3)
    return;
  if (h4[0] == H4_COMMAND && level < 2 && static_cast<uint16_t>(h4[1] | (h4[2] << 8)) == OP_HOST_COMPLETED)
    return;
  if (h4[0] == H4_EVENT && level < 2 &&
      (h4[1] == EVT_COMPLETED_PACKETS || (h4[1] == EVT_LE_META && len >= 4 && h4[3] == LE_ADVERTISING_REPORT)))
    return;
  if (h4[0] == H4_ACL && level < 2 && (len < 9 || static_cast<uint16_t>(h4[7] | (h4[8] << 8)) != L2CAP_SIGNALLING))
    return;
  char text[TRACE_BYTES * 2 + 1];
  const size_t count = std::min(len, TRACE_BYTES);
  for (size_t i = 0; i < count; i++)
    snprintf(text + i * 2, 3, "%02X", h4[i]);
  ESP_LOGI(TAG, "%s %u: %s", direction, static_cast<unsigned>(len), text);
}

static Packet *packet_(size_t len) {
  if (len == 0 || len > frame_max())
    return nullptr;
  auto *packet = static_cast<Packet *>(heap_caps_malloc(sizeof(Packet) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (packet != nullptr)
    packet->len = static_cast<uint16_t>(len);
  return packet;
}

static uint32_t credit_cost_(size_t len) { return static_cast<uint32_t>(8 + ((len + 3) & ~static_cast<size_t>(3))); }

static bool credit_room_(uint32_t cost) {
  const int32_t out = static_cast<int32_t>(s_sent.load(std::memory_order_acquire) -
                                           s_done.load(std::memory_order_acquire) -
                                           s_lost.load(std::memory_order_acquire));
  return static_cast<uint32_t>(out > 0 ? out : 0) + cost <= s_grant.load(std::memory_order_acquire);
}

static void sync_send_() {
  if (!s_credit_sync.load(std::memory_order_acquire) || s_tx == nullptr)
    return;
  const uint8_t id = static_cast<uint8_t>(s_sync_id.load(std::memory_order_acquire) + 1);
  s_sync_sent.store(s_sent.load(std::memory_order_acquire), std::memory_order_release);
  s_sync_id.store(id, std::memory_order_release);
  const uint8_t frame[2] = {static_cast<uint8_t>(MESSAGE | TYPE_SYNC), id};
  s_sent.fetch_add(credit_cost_(sizeof(frame)), std::memory_order_acq_rel);
  s_tx(frame, sizeof(frame));
}

static bool audio_frame_(uint8_t head) {
  return head == (MESSAGE | TYPE_AUDIO_PCM) || head == (MESSAGE | TYPE_AUDIO_PCM_AT);
}

static void wake_() {
  TaskHandle_t sender = s_sender.get_handle();
  if (sender != nullptr)
    xTaskNotifyGive(sender);
}

static void transmit_(const uint8_t *data, uint16_t len) {
  if (s_tx == nullptr || s_tx(data, len) != 0)
    ESP_LOGW(TAG, "Frame 0x%02X of %u bytes not sent", data[0], len);
}

static void audio_drain_() {
  Packet *packet = nullptr;
  while (s_audio != nullptr && xQueueReceive(s_audio, &packet, 0) == pdTRUE) {
    transmit_(packet->data, packet->len);
    heap_caps_free(packet);
  }
}

static bool credit_take_(size_t len) {
  if (!s_credit_on.load(std::memory_order_acquire))
    return true;
  const uint32_t cost = credit_cost_(len);
  if (!credit_room_(cost))
    return false;
  s_sent.fetch_add(cost, std::memory_order_acq_rel);
  return true;
}

static void send_(const uint8_t *data, uint16_t len) {
  const TickType_t sync_after = pdMS_TO_TICKS(CREDIT_SYNC_MS);
  TickType_t since = xTaskGetTickCount();
  while (!credit_take_(len)) {
    const TickType_t waited = xTaskGetTickCount() - since;
    if (waited >= sync_after) {
      sync_send_();
      since = xTaskGetTickCount();
      continue;
    }
    ulTaskNotifyTake(pdTRUE, sync_after - waited);
    audio_drain_();
  }
  transmit_(data, len);
}

static bool next_fits_(size_t used) {
  Packet *next = nullptr;
  return xQueuePeek(s_queue, &next, 0) == pdTRUE && next != nullptr && used + 2 + next->len <= BATCH_MAX;
}

static void send_batched_(Packet *packet) {
  uint8_t *batch = nullptr;
  if (s_peer_batch.load(std::memory_order_acquire) && 3 + packet->len <= BATCH_MAX && next_fits_(3 + packet->len))
    batch = static_cast<uint8_t *>(heap_caps_malloc(BATCH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (batch == nullptr) {
    send_(packet->data, packet->len);
    heap_caps_free(packet);
    return;
  }
  batch[0] = FRAME_BATCH;
  size_t used = 1;
  for (;;) {
    batch[used] = static_cast<uint8_t>(packet->len & 0xFF);
    batch[used + 1] = static_cast<uint8_t>(packet->len >> 8);
    std::memcpy(batch + used + 2, packet->data, packet->len);
    used += 2 + packet->len;
    heap_caps_free(packet);
    if (!next_fits_(used) || xQueueReceive(s_queue, &packet, 0) != pdTRUE || packet == nullptr)
      break;
  }
  send_(batch, static_cast<uint16_t>(used));
  heap_caps_free(batch);
}

static void sender_task(void *) {
  Packet *packet = nullptr;
  for (;;) {
    audio_drain_();
    if (xQueueReceive(s_queue, &packet, 0) != pdTRUE) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (packet != nullptr)
      send_batched_(packet);
    USBBluetooth *dongle = s_dongle;
    if (dongle != nullptr && s_resume.exchange(false, std::memory_order_acq_rel))
      dongle->resume_held();
  }
}

static bool queue_(Packet *packet) {
  if (packet == nullptr)
    return false;
  if (s_paused.load(std::memory_order_acquire)) {
    heap_caps_free(packet);
    return true;
  }
  QueueHandle_t queue = audio_frame_(packet->data[0]) ? s_audio : s_queue;
  if (queue == nullptr || xQueueSend(queue, &packet, 0) != pdTRUE) {
    heap_caps_free(packet);
    return false;
  }
  wake_();
  return true;
}

static void down_(const uint8_t *h4, size_t len) {
  USBBluetooth *dongle = s_dongle;
  if (dongle == nullptr || len < 2)
    return;
  trace_("host>", h4, len);
  xSemaphoreTake(s_down_lock, portMAX_DELAY);
  if (uxQueueMessagesWaiting(s_down) == 0 && dongle->hci_send_available() &&
      dongle->hci_write(h4, static_cast<uint16_t>(len))) {
    if (!dongle->hci_send_available() && !s_flow_off.exchange(true))
      send_available(false);
    xSemaphoreGive(s_down_lock);
    return;
  }
  Packet *packet = packet_(len);
  if (packet != nullptr) {
    std::memcpy(packet->data, h4, len);
    if (xQueueSend(s_down, &packet, 0) != pdTRUE) {
      heap_caps_free(packet);
      packet = nullptr;
    }
  }
  if (packet == nullptr)
    ESP_LOGE(TAG, "H4 packet 0x%02X of %u bytes for the dongle lost: no room", h4[0], static_cast<unsigned>(len));
  if (!s_flow_off.exchange(true))
    send_available(false);
  xSemaphoreGive(s_down_lock);
}

static bool post_start_() {
  const auto &name = App.get_name();
  uint8_t msg[1 + START_NAME_LEN];
  const size_t len = std::min<size_t>(name.size(), START_NAME_LEN);
  msg[0] = 1;
  std::memcpy(msg + 1, name.c_str(), len);
  return post(TYPE_START, msg, 1 + len);
}

static bool post_radio_start_() {
  const uint8_t start = 0;
  return post(TYPE_START, &start, sizeof(start));
}

static void dispatch_(const uint8_t *frame, uint16_t len) {
  if ((frame[0] & MESSAGE) == 0) {
    down_(frame, len);
    return;
  }
  if ((frame[0] & ~MESSAGE) == TYPE_RX_CREDIT) {
    if (len >= 5) {
      const uint32_t total = static_cast<uint32_t>(frame[1]) | (static_cast<uint32_t>(frame[2]) << 8) |
                             (static_cast<uint32_t>(frame[3]) << 16) | (static_cast<uint32_t>(frame[4]) << 24);
      if (static_cast<int32_t>(total - s_done.load(std::memory_order_acquire)) > 0)
        s_done.store(total, std::memory_order_release);
      wake_();
    } else if (len >= 3) {
      s_done.fetch_add(static_cast<uint32_t>(frame[1] | (frame[2] << 8)), std::memory_order_acq_rel);
      wake_();
    }
    return;
  }
  if ((frame[0] & ~MESSAGE) == TYPE_SYNC_ACK) {
    if (len >= 6 && frame[1] == s_sync_id.load(std::memory_order_acquire)) {
      const uint32_t total = static_cast<uint32_t>(frame[2]) | (static_cast<uint32_t>(frame[3]) << 8) |
                             (static_cast<uint32_t>(frame[4]) << 16) | (static_cast<uint32_t>(frame[5]) << 24);
      if (static_cast<int32_t>(total - s_done.load(std::memory_order_acquire)) > 0)
        s_done.store(total, std::memory_order_release);
      const int32_t lost = static_cast<int32_t>(s_sync_sent.load(std::memory_order_acquire) - total);
      const uint32_t now_lost = static_cast<uint32_t>(lost > 0 ? lost : 0);
      const uint32_t before = s_lost.exchange(now_lost, std::memory_order_acq_rel);
      if (now_lost > before)
        ESP_LOGW(TAG, "%u bytes for the C6 never arrived, credit restored", static_cast<unsigned>(now_lost - before));
      wake_();
    }
    return;
  }
  if ((frame[0] & ~MESSAGE) == TYPE_HELLO) {
    s_paused.store(false, std::memory_order_release);
    if (len >= 4) {
      s_grant.store(static_cast<uint32_t>(frame[2] | (frame[3] << 8)), std::memory_order_release);
      s_sent.store(0, std::memory_order_release);
      s_done.store(0, std::memory_order_release);
      s_lost.store(0, std::memory_order_release);
      s_credit_sync.store((frame[1] & HELLO_CREDIT_SYNC) != 0, std::memory_order_release);
      s_credit_on.store(true, std::memory_order_release);
    } else {
      s_credit_on.store(false, std::memory_order_release);
    }
    s_audio_window.store(len >= 2 && (frame[1] & HELLO_AUDIO_WINDOW) != 0, std::memory_order_release);
    s_big_frames.store(HOST_BIG_FRAMES != 0 && len >= 2 && (frame[1] & HELLO_BIG_FRAMES) != 0,
                       std::memory_order_release);
    wake_();
    const bool batch = len >= 2 && (frame[1] & HELLO_BATCH) != 0;
    s_peer_batch.store(batch, std::memory_order_release);
    const uint8_t host_flags = HOST_CREDIT_TOTAL | HOST_BIG_FRAMES;
    if (batch && !post(TYPE_BATCH_OK, &host_flags, sizeof(host_flags)))
      ESP_LOGE(TAG, "C6 not told the host takes batches");
    const bool host_ready = s_host_ready.load(std::memory_order_acquire);
    if (s_dongle != nullptr) {
      if (!post_start_())
        ESP_LOGE(TAG, "C6 not told to start its Bluetooth host");
    } else if (host_ready && !post_radio_start_()) {
      ESP_LOGE(TAG, "C6 not told to start its Bluetooth host");
    }
    if (host_ready && !post(TYPE_HOST_READY, nullptr, 0))
      ESP_LOGE(TAG, "C6 not told the host is ready");
    return;
  }
  Receiver receiver = s_receiver.load(std::memory_order_acquire);
  if (receiver != nullptr)
    receiver(frame[0] & ~MESSAGE, frame + 1, len - 1);
}

static void on_frame(const uint8_t *frame, uint16_t len, void *) {
  if (frame == nullptr || len == 0)
    return;
  if (frame[0] != FRAME_BATCH) {
    dispatch_(frame, len);
    return;
  }
  uint16_t pos = 1;
  while (static_cast<uint32_t>(pos) + 2 <= len) {
    const uint16_t item = static_cast<uint16_t>(frame[pos] | (frame[pos + 1] << 8));
    pos += 2;
    if (item == 0 || item > len - pos)
      break;
    dispatch_(frame + pos, item);
    pos += item;
  }
}

void flush_down() {
  USBBluetooth *dongle = s_dongle;
  if (dongle == nullptr || s_down == nullptr || !s_flow_off.load(std::memory_order_acquire))
    return;
  xSemaphoreTake(s_down_lock, portMAX_DELAY);
  Packet *packet = nullptr;
  while (xQueuePeek(s_down, &packet, 0) == pdTRUE) {
    if (!dongle->hci_send_available() || !dongle->hci_write(packet->data, packet->len))
      break;
    xQueueReceive(s_down, &packet, 0);
    heap_caps_free(packet);
  }
  if (uxQueueMessagesWaiting(s_down) == 0 && dongle->hci_send_available() && s_flow_off.exchange(false))
    send_available(true);
  xSemaphoreGive(s_down_lock);
}

static bool tunnel_() {
  if (!s_sender.is_created()) {
    if (s_queue == nullptr)
      s_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(Packet *), s_queue_storage, &s_queue_control);
    if (s_down == nullptr)
      s_down = xQueueCreateStatic(DOWN_DEPTH, sizeof(Packet *), s_down_storage, &s_down_control);
    if (s_down_lock == nullptr)
      s_down_lock = xSemaphoreCreateMutexStatic(&s_down_lock_control);
    if (s_audio == nullptr)
      s_audio = xQueueCreateStatic(AUDIO_DEPTH, sizeof(Packet *), s_audio_storage, &s_audio_control);
    if (!s_sender.create(sender_task, "usb_bt_c6", SENDER_STACK, nullptr, SENDER_PRIORITY, false)) {
      ESP_LOGE(TAG, "No task for the C6 tunnel");
      return false;
    }
  }
  if (s_tx == nullptr) {
    s_tx = eh_host_bt_mcu_hci_register(on_frame, nullptr);
    if (s_tx == nullptr) {
      ESP_LOGE(TAG, "C6 HCI channel not ready");
      return false;
    }
  }
  return true;
}

bool start(USBBluetooth *dongle) {
  if (!tunnel_())
    return false;
  s_dongle = dongle;
  if (!post_start_()) {
    ESP_LOGE(TAG, "C6 not told to start its Bluetooth host");
    return false;
  }
  ESP_LOGV(TAG, "USB dongle handed to the C6's Bluetooth host");
  return true;
}

void stop() {
  if (s_down_lock == nullptr)
    return;
  xSemaphoreTake(s_down_lock, portMAX_DELAY);
  const bool was_up = s_dongle != nullptr;
  s_dongle = nullptr;
  Packet *packet = nullptr;
  while (xQueueReceive(s_down, &packet, 0) == pdTRUE)
    heap_caps_free(packet);
  s_flow_off.store(false, std::memory_order_release);
  xSemaphoreGive(s_down_lock);
  if (was_up && !post_radio_start_())
    ESP_LOGE(TAG, "C6 not told the dongle left");
}

bool up(const uint8_t *h4, size_t len) {
  trace_("dongle>", h4, len);
  Packet *packet = packet_(len);
  if (packet == nullptr)
    return false;
  std::memcpy(packet->data, h4, len);
  return queue_(packet);
}

void want_resume() {
  s_resume.store(true, std::memory_order_release);
  Packet *wake = nullptr;
  if (s_queue != nullptr)
    xQueueSend(s_queue, &wake, 0);
  wake_();
}

void send_available(bool available) {
  const uint8_t value = available ? 1 : 0;
  post(TYPE_FLOW, &value, sizeof(value));
}

bool post(uint8_t type, const uint8_t *data, size_t len) {
  Packet *packet = packet_(len > 0 ? len + 1 : 2);
  if (packet == nullptr)
    return false;
  packet->data[0] = static_cast<uint8_t>(MESSAGE | type);
  if (len > 0)
    std::memcpy(packet->data + 1, data, len);
  else
    packet->data[1] = 0;
  return queue_(packet);
}

bool post_filled(uint8_t type, size_t len, size_t (*fill)(uint8_t *dst, size_t len, void *ctx), void *ctx) {
  Packet *packet = packet_(len + 1);
  if (packet == nullptr)
    return false;
  const size_t filled = fill(packet->data + 1, len, ctx);
  if (filled == 0 || filled > len) {
    heap_caps_free(packet);
    return false;
  }
  packet->data[0] = static_cast<uint8_t>(MESSAGE | type);
  packet->len = static_cast<uint16_t>(filled + 1);
  return queue_(packet);
}

void set_trace(uint8_t level) { s_trace.store(level, std::memory_order_relaxed); }

void set_paused(bool paused) { s_paused.store(paused, std::memory_order_release); }

bool audio_window() { return s_audio_window.load(std::memory_order_acquire); }

size_t frame_max() { return s_big_frames.load(std::memory_order_acquire) ? FRAME_MAX_BIG : FRAME_MAX; }

void set_host_ready() {
  if (s_host_ready.exchange(true, std::memory_order_acq_rel))
    return;
  if (s_tx == nullptr)
    return;
  if (s_dongle == nullptr && !post_radio_start_())
    ESP_LOGE(TAG, "C6 not told to start its Bluetooth host");
  if (!post(TYPE_HOST_READY, nullptr, 0))
    ESP_LOGE(TAG, "C6 not told the host is ready");
}

void set_receiver(Receiver receiver) {
  s_receiver.store(receiver, std::memory_order_release);
  tunnel_();
}

}

#endif
