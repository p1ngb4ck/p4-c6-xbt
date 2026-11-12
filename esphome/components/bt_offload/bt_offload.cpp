#include "bt_offload.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#ifdef USE_BT_OFFLOAD_SPEAKER
#include "speaker/bt_offload_speaker.h"
#endif
#ifdef USE_BT_PAIRING_SELECT
#include "esphome/components/bt_host/select/bt_host_select.h"
#endif
#ifdef USE_BT_OFFLOAD_NUMBER
#include "number/bt_offload_delay_number.h"
#endif
#include "esphome/components/usb_bluetooth/c6_offload.h"
#ifdef USE_BT_OFFLOAD_PACKAGE_MANAGER
#include "esphome/components/package_manager/package_manager.h"
#endif
#include "esp_heap_caps.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

namespace esphome::bt_offload {

static const char *const TAG = "bt_offload";

static constexpr uint8_t MSG_HID_OPEN = 0x10;
static constexpr uint8_t MSG_HID_INPUT = 0x11;
static constexpr uint8_t MSG_HID_CLOSE = 0x12;
static constexpr uint8_t TYPE_HID_OUTPUT = 0x13;
static constexpr uint8_t TYPE_HID_SET_REPORT = 0x14;
static constexpr uint8_t TYPE_HID_GET_FEATURE = 0x15;
static constexpr uint8_t MSG_HID_FEATURE = 0x16;
static constexpr uint8_t MSG_C6_LOG = 0x17;
static constexpr uint8_t TYPE_C6_LOG_SET = 0x18;
static constexpr uint8_t C6_LOG_MAX = 3;
static constexpr uint8_t C6_LOG_TRACE = 4;
static constexpr uint32_t C6_STATE_SAFE = 0x01;
static constexpr uint32_t C6_STATE_DONGLE = 0x02;
static constexpr uint32_t C6_STATE_READY = 0x04;
static constexpr uint32_t C6_STATE_REQUESTED = 0x08;
static constexpr uint8_t MSG_HID_INPUT_RAW = 0x19;
static constexpr size_t HID_REPORT_MAX = 256;
static constexpr uint8_t REPORT_TYPE_INPUT = 1;
static constexpr uint8_t REPORT_TYPE_OUTPUT = 2;
static constexpr uint8_t REPORT_TYPE_FEATURE = 3;
static constexpr uint8_t MSG_FOUND = 0x21;
static constexpr uint8_t MSG_AUDIO_STATE = 0x33;
static constexpr uint8_t MSG_AUDIO_CREDIT = 0x34;
static constexpr uint8_t MSG_AUDIO_WINDOW = 0x39;
static constexpr uint8_t MSG_STATUS = 0x50;
static constexpr uint8_t MSG_STATUS_PACKED = 0x51;
static constexpr uint8_t MSG_REPORT = 0x52;
static constexpr uint8_t REPORT_STACKS = 1;
static constexpr uint8_t REPORT_HEAP_OWNERS = 2;
static constexpr uint8_t REPORT_BLOCKS = 3;
static constexpr uint8_t REPORT_MARKS = 4;
static constexpr uint8_t REPORT_PARTITIONS = 5;
static constexpr uint8_t C6_LOG_FOLLOW = 0xFF;
static constexpr uint8_t C6_LOG_AT_VERY_VERBOSE = 5;
static constexpr uint8_t MSG_RECORDS = 0x24;
static constexpr uint8_t MSG_NVS_ENTRY = 0x68;
static constexpr uint8_t MSG_NVS_VALUE = 0x69;
static constexpr uint8_t MSG_NVS_DONE = 0x6A;
static constexpr uint8_t TYPE_STATUS = 0x50;
static constexpr uint8_t TYPE_FORGET = 0x26;
static constexpr uint8_t TYPE_NVS_LIST = 0x60;
static constexpr uint8_t TYPE_NVS_GET = 0x61;
static constexpr uint8_t TYPE_NVS_SET = 0x62;
static constexpr uint8_t TYPE_NVS_ERASE = 0x63;
#ifdef USE_BT_OFFLOAD_BOND_STORE
static constexpr uint8_t TYPE_NVS_RELOAD = 0x64;
static const char *const BOND_DIR = "/sdcard/bluetooth";
static const char *const BOND_FILE = "/sdcard/bluetooth/c6_bonds.bin";
enum : uint8_t { BOND_IDLE, BOND_LIST, BOND_GET, BOND_WRITE, BOND_READ, BOND_SET };
#endif
static constexpr size_t RECORD_WIRE = 10 + BTOffload::RECORD_NAME + 4;
static constexpr uint8_t TYPE_SCAN = 0x20;
static constexpr uint8_t TYPE_CONNECT = 0x22;
static constexpr uint8_t TYPE_AUDIO_CONNECT = 0x30;
static constexpr uint8_t TYPE_AUDIO_DISCONNECT = 0x31;
static constexpr uint8_t TYPE_AUDIO_VOLUME = 0x35;
static constexpr uint8_t MSG_SCAN_STATE = 0x23;
static constexpr uint8_t MSG_PAIR_EVENT = 0x28;
static constexpr uint8_t PAIR_PASSKEY = 1;
static constexpr uint8_t PAIR_CONFIRM = 2;
static constexpr uint8_t PAIR_DONE = 3;
static constexpr uint8_t PAIR_PIN = 5;
static constexpr uint8_t PAIR_CODE_LE = 7;
static constexpr uint8_t TYPE_PAIR_REPLY = 0x2C;
static constexpr uint32_t PIN_PREF_KEY = 0x42544F50;
static constexpr uint8_t TYPE_CANCEL_SCAN = 0x25;
static constexpr uint8_t TYPE_HID_DISCONNECT = 0x27;
static constexpr uint8_t TYPE_AUDIO_PAUSE = 0x36;
static constexpr uint8_t TYPE_AUDIO_INTERVAL = 0x37;
static constexpr uint8_t TYPE_BROADCAST_ADD = 0x29;
static constexpr uint8_t MSG_BROADCAST_DATA = 0x2A;
static constexpr uint8_t MSG_ERROR = 0x2B;
static constexpr uint8_t FOUND_BROADCAST = 2;

static BTOffload *g_offload = nullptr;

void OffloadEvent::release() {
  if (this->large != nullptr) {
    heap_caps_free(this->large);
    this->large = nullptr;
  }
  this->len = 0;
}

static void forward(uint8_t type, const uint8_t *data, size_t len) {
  BTOffload *self = g_offload;
  if (self != nullptr && data != nullptr && len > 0 && len <= UINT16_MAX)
    self->receive_(type, data, len);
}

void BTOffload::receive_(uint32_t msg_id, const uint8_t *data, size_t len) {
#ifdef USE_BT_OFFLOAD_SPEAKER
  if (msg_id == MSG_AUDIO_CREDIT) {
    BTOffloadSpeaker *speaker = this->speaker_;
    if (speaker != nullptr && len >= 4) {
      uint32_t bytes;
      std::memcpy(&bytes, data, sizeof(bytes));
      speaker->on_credit(bytes);
    }
    return;
  }
  if (msg_id == MSG_AUDIO_WINDOW) {
    BTOffloadSpeaker *speaker = this->speaker_;
    if (speaker != nullptr && len >= 8) {
      uint32_t window[2];
      std::memcpy(window, data, sizeof(window));
      speaker->on_window(window[0], window[1]);
    }
    return;
  }
#endif
  OffloadEvent *event = this->pool_.allocate();
  if (event == nullptr)
    return;
  event->msg_id = 0;
  event->len = 0;
  if (len > OffloadEvent::INLINE_MAX)
    event->large = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (len <= OffloadEvent::INLINE_MAX || event->large != nullptr) {
    event->msg_id = msg_id;
    event->len = static_cast<uint16_t>(len);
    std::memcpy(event->large != nullptr ? event->large : event->inline_data, data, len);
  }
  this->queue_.push(event);
  this->enable_loop_soon_any_context();
}

void BTOffload::loop() {
  OffloadEvent *event;
  while ((event = this->queue_.pop()) != nullptr) {
    switch (event->msg_id) {
      case MSG_HID_OPEN:
        this->on_open_(event->data(), event->len);
        break;
      case MSG_HID_INPUT:
        this->on_input_(event->data(), event->len);
        break;
      case MSG_HID_CLOSE:
        this->on_close_(event->data(), event->len);
        break;
      case MSG_HID_FEATURE:
        this->on_feature_(event->data(), event->len);
        break;
      case MSG_HID_INPUT_RAW:
        this->on_input_raw_(event->data(), event->len);
        break;
      case MSG_C6_LOG: {
        size_t line = event->len;
        while (line > 0 && (event->data()[line - 1] == '\n' || event->data()[line - 1] == '\r'))
          line--;
        ESP_LOGW(TAG, "C6 log: %.*s", static_cast<int>(line), reinterpret_cast<const char *>(event->data()));
        break;
      }
      case MSG_FOUND:
        this->on_found_(event->data(), event->len);
        break;
      case MSG_SCAN_STATE:
        this->on_scan_state_(event->data(), event->len);
        break;
      case MSG_PAIR_EVENT:
        this->on_pair_event_(event->data(), event->len);
        break;
      case MSG_AUDIO_STATE:
        this->on_audio_state_(event->data(), event->len);
        break;
      case MSG_STATUS:
        this->on_status_(event->data(), event->len);
        break;
      case MSG_STATUS_PACKED:
        this->on_status_packed_(event->data(), event->len);
        break;
      case MSG_REPORT:
        this->on_report_(event->data(), event->len);
        break;
      case MSG_RECORDS:
        this->on_records_(event->data(), event->len);
        break;
      case MSG_NVS_ENTRY:
        this->on_nvs_entry_(event->data(), event->len);
        break;
      case MSG_NVS_VALUE:
        this->on_nvs_value_(event->data(), event->len);
        break;
      case MSG_NVS_DONE:
        this->on_nvs_done_(event->data(), event->len);
        break;
      case MSG_BROADCAST_DATA:
        this->on_broadcast_(event->data(), event->len);
        break;
      case MSG_ERROR:
        if (event->len >= sizeof(int32_t)) {
          int32_t err;
          memcpy(&err, event->data(), sizeof(err));
          ESP_LOGE(TAG, "C6: Bluetooth not ready: %s", esp_err_to_name(err));
        }
        break;
      default:
        break;
    }
    this->pool_.release(event);
  }
  this->disable_loop();
}

static std::string format_address(const uint8_t *a) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

static bool parse_address(const std::string &address, uint8_t *out) {
  unsigned value[6];
  if (std::sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &value[0], &value[1], &value[2], &value[3], &value[4],
                  &value[5]) != 6)
    return false;
  for (size_t i = 0; i < 6; i++)
    out[i] = static_cast<uint8_t>(value[i]);
  return true;
}

void BTOffload::setup() {
  this->disable_loop();
  g_offload = this;
  this->records_ = static_cast<Record *>(
      heap_caps_calloc(RECORDS_MAX, sizeof(Record), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  for (uint8_t i = 0; i < this->max_hid_devices_; i++) {
    void *memory = heap_caps_malloc(sizeof(OffloadSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr)
      break;
    auto *slot = new (memory) OffloadSlot();
    slot->set_parent(this);
    slot->set_layout(this->get_layout());
    this->add_slot_(slot);
  }
  this->pin_pref_ = global_preferences->make_preference<char[PIN_MAX + 1]>(PIN_PREF_KEY, true);
  char stored[PIN_MAX + 1];
  if (this->pin_pref_.load(&stored)) {
    stored[PIN_MAX] = '\0';
    std::memcpy(this->preset_pin_, stored, sizeof(stored));
  }
  usb_bluetooth::c6_offload::set_receiver(forward);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  this->c6_log_wanted_ = this->c6_log_override_ != C6_LOG_FOLLOW ? this->c6_log_override_ : C6_LOG_AT_VERY_VERBOSE;
#else
  this->c6_log_wanted_ = 0;
#endif
  const uint8_t log_set[2] = {this->c6_log_wanted_, this->c6_trace_wanted_};
  this->command_(TYPE_C6_LOG_SET, log_set, sizeof(log_set));
  this->command_(MSG_RECORDS, nullptr, 0);
#ifdef USE_BT_OFFLOAD_SENSOR
  if (this->status_interval_ > 0) {
    this->set_interval("c6_status", this->status_interval_, [this]() {
      this->status_quiet_ = true;
      this->status_brief_ = true;
      if (!this->command_(TYPE_STATUS, nullptr, 0))
        this->status_quiet_ = false;
    });
  }
#endif
#ifdef USE_BT_OFFLOAD_PACKAGE_MANAGER
  if (this->package_manager_ != nullptr) {
    auto *package_manager = this->package_manager_;
    package_manager->add_on_change_callback([package_manager]() {
      if (package_manager->boot_settled())
        usb_bluetooth::c6_offload::set_host_ready();
    });
    if (package_manager->boot_settled())
      usb_bluetooth::c6_offload::set_host_ready();
    return;
  }
#endif
  usb_bluetooth::c6_offload::set_host_ready();
}

bool OffloadSlot::send_output_report(const uint8_t *data, size_t len) {
  return this->connected && len > 0 && this->parent_->hid_report(TYPE_HID_OUTPUT, this->address, -1, data, len);
}

bool OffloadSlot::set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) {
  if (!this->connected || len == 0)
    return false;
  const uint8_t type = kind == hid::HIDReportKind::FEATURE  ? REPORT_TYPE_FEATURE
                       : kind == hid::HIDReportKind::OUTPUT ? REPORT_TYPE_OUTPUT
                                                            : REPORT_TYPE_INPUT;
  return this->parent_->hid_report(TYPE_HID_SET_REPORT, this->address, type, data, len);
}

bool OffloadSlot::request_feature_report(uint8_t id, uint16_t len) {
  if (!this->connected || this->feature_id_ != 0)
    return false;
  const uint8_t size[2] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
  if (!this->parent_->hid_report(TYPE_HID_GET_FEATURE, this->address, id, size, sizeof(size)))
    return false;
  this->feature_id_ = id;
  return true;
}

Component *OffloadSlot::hid_component() { return this->parent_; }

bool BTOffload::hid_report(uint8_t type, const uint8_t *address, int prefix, const uint8_t *data, size_t len) {
  if (len > HID_REPORT_MAX)
    return false;
  uint8_t msg[7 + HID_REPORT_MAX];
  size_t at = 6;
  std::memcpy(msg, address, at);
  if (prefix >= 0)
    msg[at++] = static_cast<uint8_t>(prefix);
  std::memcpy(msg + at, data, len);
  return this->command_(type, msg, at + len);
}

bool BTOffload::set_c6_log(uint8_t level) {
  usb_bluetooth::c6_offload::set_trace(level >= C6_LOG_TRACE ? level - C6_LOG_TRACE + 1 : 0);
  this->c6_log_wanted_ = std::min(level, C6_LOG_MAX);
  this->c6_trace_wanted_ = level >= C6_LOG_TRACE ? 1 : 0;
  const uint8_t log_set[2] = {this->c6_log_wanted_, this->c6_trace_wanted_};
  return this->command_(TYPE_C6_LOG_SET, log_set, sizeof(log_set));
}

void BTOffload::on_input_raw_(const uint8_t *data, size_t len) {
  if (len < 7)
    return;
  OffloadSlot *slot = this->slot_(data);
  if (slot == nullptr || slot->pad_report(data + 6, len - 6) || slot->map == nullptr)
    return;
  const uint8_t *report = data + 6;
  size_t length = len - 6;
  hid::HIDReportSource source;
  if (slot->map->uses_report_ids()) {
    source.has_report_id = true;
    source.report_id = report[0];
    report++;
    length--;
  }
  slot->process_hid_values(slot->map->parse(source, report, length));
}

void BTOffload::on_feature_(const uint8_t *data, size_t len) {
  if (len < 6)
    return;
  OffloadSlot *slot = this->slot_(data);
  if (slot != nullptr)
    slot->pad_feature(data + 6, len - 6);
}

void BTOffload::add_slot_(OffloadSlot *slot) {
  this->slots_.push_back(slot);
  slot->add_on_key_event_callback([this, slot](uint16_t page, uint16_t usage, bool pressed) {
    this->device_layout_ = this->layout_of_(slot->address);
    this->forward_source_ = slot->key_source();
    this->forward_label_ = slot->key_source_label();
    this->key_(page, usage, pressed);
    this->forward_source_ = 0;
    this->forward_label_ = nullptr;
    this->device_layout_ = nullptr;
  });
  slot->add_on_pointer_callback([this](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
    this->buttons_ = buttons;
    this->pointer_cb_.call(dx, dy, wheel, pan, buttons);
  });
  slot->add_on_control_callback(
      [this](uint16_t page, uint16_t usage, int32_t value) { this->control_cb_.call(page, usage, value); });
}

OffloadSlot *BTOffload::slot_(const uint8_t *address) const {
  for (auto *slot : this->slots_) {
    if (slot->connected && std::memcmp(slot->address, address, 6) == 0)
      return slot;
  }
  return nullptr;
}

const hid::KeyboardLayout *BTOffload::layout_of_(const uint8_t *address) const {
  for (const auto &entry : this->layouts_) {
    if (std::memcmp(entry.address, address, 6) == 0)
      return entry.layout;
  }
  return nullptr;
}

const BTOffload::Record *BTOffload::find_record(const uint8_t *address) const {
  for (size_t i = 0; i < this->record_count_; i++) {
    if (std::memcmp(this->records_[i].address, address, 6) == 0)
      return &this->records_[i];
  }
  return nullptr;
}

const BTOffload::Record *BTOffload::audio_record() const {
  for (size_t i = 0; i < this->record_count_; i++) {
    if (this->records_[i].kind & KIND_AUDIO)
      return &this->records_[i];
  }
  return nullptr;
}

uint8_t BTOffload::get_device_kinds() const {
  uint8_t kinds = 0;
  for (const auto *slot : this->slots_)
    kinds |= slot->get_device_kinds();
  return kinds;
}

void BTOffload::list_devices(std::vector<hid::InputDevice> &out) const {
  for (size_t i = 0; i < this->record_count_; i++) {
    const Record &record = this->records_[i];
    if ((record.kind & KIND_HID) == 0)
      continue;
    const OffloadSlot *slot = this->slot_(record.address);
    const std::string mac = format_address(record.address);
    out.push_back({std::string("bt:") + mac, record.name[0] != '\0' ? std::string(record.name) : mac,
                   slot != nullptr, this->record_kinds(record)});
  }
}

hid::HIDInput *BTOffload::device_input(const std::string &key) {
  uint8_t address[6];
  if (key.rfind("bt:", 0) != 0 || !parse_address(key.substr(3), address))
    return nullptr;
  return this->slot_(address);
}

void BTOffload::set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) {
  uint8_t address[6];
  if (key.rfind("bt:", 0) != 0 || !parse_address(key.substr(3), address))
    return;
  for (auto it = this->layouts_.begin(); it != this->layouts_.end(); ++it) {
    if (std::memcmp(it->address, address, 6) == 0) {
      if (layout == nullptr) {
        this->layouts_.erase(it);
      } else {
        it->layout = layout;
      }
      return;
    }
  }
  if (layout != nullptr) {
    DeviceLayout entry{};
    std::memcpy(entry.address, address, 6);
    entry.layout = layout;
    this->layouts_.push_back(entry);
  }
}

bool BTOffload::address_command_(uint8_t type, const std::string &address) {
  uint8_t bda[6];
  if (!parse_address(address, bda)) {
    ESP_LOGW(TAG, "%s is no Bluetooth address", address.c_str());
    return false;
  }
  return this->command_(type, bda, sizeof(bda));
}

bool BTOffload::cancel_scan() { return this->command_(TYPE_CANCEL_SCAN, nullptr, 0); }

bool BTOffload::disconnect(const std::string &address) { return this->address_command_(TYPE_HID_DISCONNECT, address); }

bool BTOffload::is_connected(const std::string &address) const {
  uint8_t bda[6];
  if (!parse_address(address, bda))
    return false;
  if (this->slot_(bda) != nullptr)
    return true;
  const Record *record = this->find_record(bda);
  if (record != nullptr && (record->kind & KIND_BROADCAST))
    return true;
  return this->audio_connected_ && std::memcmp(this->audio_address_.data(), bda, 6) == 0;
}

void BTOffload::on_broadcast_(const uint8_t *data, size_t len) {
  if (len < 6)
    return;
  const std::string address = format_address(data);
  for (size_t at = 6; at + 1 < len;) {
    const size_t field = data[at];
    if (field == 0 || at + 1 + field > len)
      break;
    const uint8_t type = data[at + 1];
    const uint8_t *value = data + at + 2;
    const size_t value_len = field - 1;
    if (type == 0xFF && value_len >= 2) {
      const uint16_t id = static_cast<uint16_t>(value[0] | (value[1] << 8));
      this->manufacturer_cb_.call(std::vector<uint8_t>(value + 2, value + value_len), id, address);
    } else if (type == 0x16 && value_len >= 2) {
      const uint16_t uuid = static_cast<uint16_t>(value[0] | (value[1] << 8));
      this->service_cb_.call(std::vector<uint8_t>(value + 2, value + value_len), uuid, address);
    } else if (type == 0x20 && value_len >= 4) {
      this->service_cb_.call(std::vector<uint8_t>(value + 4, value + value_len), 0, address);
    } else if (type == 0x21 && value_len >= 16) {
      this->service_cb_.call(std::vector<uint8_t>(value + 16, value + value_len), 0, address);
    }
    at += 1 + field;
  }
}

static uint32_t delay_key(const std::array<uint8_t, 6> &address) {
  return fnv1_hash(std::string("bt_offload_delay_") + format_address(address.data()));
}

void BTOffload::load_delay_() {
  ESPPreferenceObject pref = global_preferences->make_preference<int32_t>(delay_key(this->audio_address_));
  int32_t ms = 0;
  this->delay_ms_ =
      pref.load(&ms) ? std::clamp<int32_t>(ms, MIN_OUTPUT_DELAY_MS, static_cast<int32_t>(MAX_OUTPUT_DELAY_MS)) : 0;
  this->publish_delay_();
}

void BTOffload::set_output_delay_ms(int32_t ms) {
  this->delay_ms_ = std::clamp<int32_t>(ms, MIN_OUTPUT_DELAY_MS, static_cast<int32_t>(MAX_OUTPUT_DELAY_MS));
  this->publish_delay_();
  if (!this->audio_connected_)
    return;
  ESPPreferenceObject pref = global_preferences->make_preference<int32_t>(delay_key(this->audio_address_));
  pref.save(&this->delay_ms_);
}

bool BTOffload::set_audio_paused(bool paused) {
  this->audio_paused_ = paused;
  if (!this->active_)
    return true;
  const uint8_t value = paused ? 1 : 0;
  return this->command_(TYPE_AUDIO_PAUSE, &value, 1);
}

bool BTOffload::set_audio_paging_interval(uint16_t seconds) {
  this->paging_interval_ = seconds;
  if (!this->active_)
    return true;
  const uint8_t value[2] = {static_cast<uint8_t>(seconds & 0xFF), static_cast<uint8_t>(seconds >> 8)};
  return this->command_(TYPE_AUDIO_INTERVAL, value, sizeof(value));
}

void BTOffload::on_pair_event_(const uint8_t *data, size_t len) {
  if (len < 11)
    return;
  uint32_t value;
  std::memcpy(&value, data + 7, sizeof(value));
  std::string name = format_address(data);
  for (const auto &found : this->found_) {
    if (std::memcmp(found.address, data, 6) == 0 && !found.name.empty())
      name = found.name;
  }
  const Record *record = this->find_record(data);
  if (record != nullptr && record->name[0] != '\0')
    name = record->name;
  if (data[6] >= PAIR_PIN && data[6] <= PAIR_CODE_LE) {
    this->on_code_request_(data, name);
    return;
  }
  char *text = this->status_text_;
  const size_t size = sizeof(this->status_text_);
  if (data[6] == PAIR_PASSKEY) {
    snprintf(text, size, "Type %06" PRIu32 " on %s and press Enter", value, name.c_str());
  } else if (data[6] == PAIR_CONFIRM) {
    snprintf(text, size, "Pairing %s: code %06" PRIu32, name.c_str(), value);
  } else if (data[6] == PAIR_DONE) {
    snprintf(text, size, "Paired with %s", name.c_str());
  } else {
    snprintf(text, size, "Pairing with %s failed (%" PRIu32 ")", name.c_str(), value);
    this->report_status_();
  }
  ESP_LOGI(TAG, "%s", text);
  this->status_cb_.call(std::string(text));
  if (data[6] == PAIR_DONE)
    this->connected_cb_.call(name);
}

void BTOffload::on_code_request_(const uint8_t *data, const std::string &name) {
  std::memcpy(this->pending_address_, data, sizeof(this->pending_address_));
  this->pending_kind_ = data[6];
  const bool pin = this->pending_kind_ == PAIR_PIN;
  if (this->has_code_ui_) {
    this->code_cb_.call(name, pin, pin ? std::string(this->preset_pin_) : std::string());
    return;
  }
  if (pin && this->preset_pin_[0] != '\0') {
    this->pair_reply(this->preset_pin_);
    return;
  }
  snprintf(this->status_text_, sizeof(this->status_text_), "%s asks for a %s", name.c_str(), pin ? "PIN" : "code");
  ESP_LOGI(TAG, "%s", this->status_text_);
  this->status_cb_.call(std::string(this->status_text_));
}

bool BTOffload::send_pair_reply_(bool accept, const std::string &code) {
  if (this->pending_kind_ == 0 || code.size() > PIN_MAX)
    return false;
  uint8_t msg[9 + PIN_MAX];
  std::memcpy(msg, this->pending_address_, sizeof(this->pending_address_));
  msg[6] = this->pending_kind_;
  msg[7] = accept ? 1 : 0;
  msg[8] = static_cast<uint8_t>(code.size());
  std::memcpy(msg + 9, code.data(), code.size());
  this->pending_kind_ = 0;
  return this->command_(TYPE_PAIR_REPLY, msg, 9 + code.size());
}

bool BTOffload::pair_reply(const std::string &code) { return this->send_pair_reply_(true, code); }

bool BTOffload::pair_cancel() { return this->send_pair_reply_(false, ""); }

void BTOffload::set_preset_pin(const std::string &pin) {
  if (pin.size() > PIN_MAX)
    return;
  std::memcpy(this->preset_pin_, pin.c_str(), pin.size() + 1);
  this->pin_pref_.save(&this->preset_pin_);
}

void BTOffload::on_scan_state_(const uint8_t *data, size_t len) {
  if (len < 1)
    return;
  this->scanning_ = data[0] != 0;
  this->scanned_cb_.call();
}

bool BTOffload::forget(const std::string &address) {
  uint8_t bda[6];
  if (!parse_address(address, bda)) {
    ESP_LOGW(TAG, "%s is no Bluetooth address", address.c_str());
    return false;
  }
  return this->forget_address(bda);
}

void BTOffload::on_records_(const uint8_t *data, size_t len) {
  if (this->records_ == nullptr || len < 1)
    return;
  const size_t count = std::min<size_t>(std::min<size_t>(data[0], RECORDS_MAX), (len - 1) / RECORD_WIRE);
  for (size_t i = 0; i < count; i++) {
    const uint8_t *in = data + 1 + i * RECORD_WIRE;
    Record &rec = this->records_[i];
    std::memcpy(rec.address, in, 6);
    rec.transport = in[6];
    rec.addr_type = in[7];
    rec.kind = in[8];
    rec.slot = static_cast<uint8_t>(i + 1);
    const size_t name_len = std::min<size_t>(in[9], RECORD_NAME);
    std::memcpy(rec.name, in + 10, name_len);
    rec.name[name_len] = '\0';
    std::memcpy(&rec.device_class, in + 10 + RECORD_NAME, sizeof(rec.device_class));
  }
  this->record_count_ = count;
  const bool first = !this->active_;
#ifdef USE_BT_OFFLOAD_BOND_STORE
  const uint32_t sig = this->bond_signature_();
  const bool forgotten = this->bond_forget_;
  this->bond_forget_ = false;
  if (first) {
    this->bond_sig_ = sig;
    if (count == 0) {
      this->bond_restore_();
    } else {
      this->bond_save_();
    }
  } else if (sig != this->bond_sig_) {
    this->bond_sig_ = sig;
    if (count == 0 && !forgotten) {
      ESP_LOGW(TAG, "The C6 lost its bonds: restoring them from %s", BOND_FILE);
      this->bond_restore_();
    } else {
      this->bond_save_();
    }
  }
#endif
  if (first)
    this->report_status_();
  this->active_ = true;
  if (first) {
    this->set_audio_paused(this->audio_paused_);
    this->set_audio_paging_interval(this->paging_interval_);
  }
  this->records_cb_.call();
  this->devices_cb_.call();
}

bool BTOffload::nvs_key_command_(uint8_t type, uint8_t space, const std::string &key, uint8_t value_type,
                                 const uint8_t *data, size_t len) {
  if (key.size() > 15 || 2 + key.size() + 1 + len > usb_bluetooth::c6_offload::frame_max() - 1)
    return false;
  uint8_t *msg = static_cast<uint8_t *>(heap_caps_malloc(2 + key.size() + 1 + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (msg == nullptr)
    return false;
  size_t at = 0;
  msg[at++] = space;
  msg[at++] = static_cast<uint8_t>(key.size());
  std::memcpy(msg + at, key.data(), key.size());
  at += key.size();
  if (type == TYPE_NVS_SET) {
    msg[at++] = value_type;
    if (len > 0)
      std::memcpy(msg + at, data, len);
    at += len;
  }
  const bool ok = this->command_(type, msg, at);
  heap_caps_free(msg);
  return ok;
}

bool BTOffload::nvs_list(uint8_t space) { return space <= SPACE_BLUEDROID && this->command_(TYPE_NVS_LIST, &space, 1); }

bool BTOffload::nvs_get(uint8_t space, const std::string &key) {
  return space <= SPACE_BLUEDROID && !key.empty() && this->nvs_key_command_(TYPE_NVS_GET, space, key, 0, nullptr, 0);
}

bool BTOffload::nvs_set(uint8_t space, const std::string &key, uint8_t type, const uint8_t *data, size_t len) {
  return space <= SPACE_BLUEDROID && !key.empty() && this->nvs_key_command_(TYPE_NVS_SET, space, key, type, data, len);
}

bool BTOffload::nvs_erase(uint8_t space, const std::string &key) {
  return space <= SPACE_BLUEDROID && this->nvs_key_command_(TYPE_NVS_ERASE, space, key, 0, nullptr, 0);
}

void BTOffload::on_nvs_entry_(const uint8_t *data, size_t len) {
  if (len < 3 || len < 3u + data[2])
    return;
#ifdef USE_BT_OFFLOAD_BOND_STORE
  if (this->bond_state_ == BOND_LIST && data[0] == this->bond_at_ && data[2] <= BOND_KEY_SIZE - 3 &&
      this->bond_key_count_ < BOND_KEYS_MAX) {
    uint8_t *key = this->bond_buffer_ + this->bond_key_count_++ * BOND_KEY_SIZE;
    std::memcpy(key, data, 3 + data[2]);
  }
#endif
  this->nvs_entry_cb_.call(data[0], data[1], std::string(reinterpret_cast<const char *>(data + 3), data[2]));
}

void BTOffload::on_nvs_value_(const uint8_t *data, size_t len) {
  if (len < 3 || len < 5u + data[2])
    return;
  const size_t key_len = data[2];
  const size_t value_len = data[3 + key_len] | (data[4 + key_len] << 8);
  if (len < 5 + key_len + value_len)
    return;
#ifdef USE_BT_OFFLOAD_BOND_STORE
  if (this->bond_state_ == BOND_GET) {
    const size_t entry = 5 + key_len + value_len;
    if (this->bond_len_ + entry <= BOND_IMAGE_MAX) {
      std::memcpy(this->bond_buffer_ + BOND_KEYS_MAX * BOND_KEY_SIZE + this->bond_len_, data, entry);
      this->bond_len_ += entry;
    } else {
      ESP_LOGW(TAG, "Bond store: %.*s does not fit, not saved", static_cast<int>(key_len),
               reinterpret_cast<const char *>(data + 3));
    }
    this->bond_next_get_();
  }
#endif
  this->nvs_value_cb_.call(data[0], data[1], std::string(reinterpret_cast<const char *>(data + 3), key_len),
                           data + 5 + key_len, value_len);
}

void BTOffload::on_nvs_done_(const uint8_t *data, size_t len) {
  if (len < 5)
    return;
  int32_t err;
  std::memcpy(&err, data + 1, sizeof(err));
#ifdef USE_BT_OFFLOAD_BOND_STORE
  if (this->bond_state_ == BOND_LIST && data[0] == TYPE_NVS_LIST) {
    if (err != 0) {
      this->bond_finish_();
    } else if (this->bond_at_ == SPACE_RECORDS) {
      this->bond_at_ = SPACE_BLUEDROID;
      if (this->nvs_list(SPACE_BLUEDROID)) {
        this->bond_wait_();
      } else {
        this->bond_finish_();
      }
    } else {
      this->bond_state_ = BOND_GET;
      this->bond_key_next_ = 0;
      this->bond_next_get_();
    }
  } else if (this->bond_state_ == BOND_GET && data[0] == TYPE_NVS_GET) {
    this->bond_next_get_();
  } else if (this->bond_state_ == BOND_SET && data[0] == TYPE_NVS_SET) {
    this->bond_next_set_();
  }
#endif
  this->nvs_done_cb_.call(data[0], err);
}

#ifdef USE_BT_OFFLOAD_BOND_STORE
uint32_t BTOffload::bond_signature_() const {
  uint32_t sig = 2166136261u ^ static_cast<uint32_t>(this->record_count_);
  for (size_t i = 0; i < this->record_count_; i++) {
    for (uint8_t byte : this->records_[i].address)
      sig = (sig ^ byte) * 16777619u;
  }
  return sig;
}

void BTOffload::bond_save_() {
  if (this->bond_state_ != BOND_IDLE) {
    this->bond_again_ = true;
    return;
  }
  if (storage::global_storage_worker == nullptr)
    return;
  this->bond_buffer_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(BOND_BUFFER);
  if (this->bond_buffer_ == nullptr)
    return;
  this->bond_key_count_ = 0;
  this->bond_len_ = 0;
  this->bond_at_ = SPACE_RECORDS;
  this->bond_state_ = BOND_LIST;
  if (!this->nvs_list(SPACE_RECORDS)) {
    this->bond_finish_();
    return;
  }
  this->bond_wait_();
}

void BTOffload::bond_wait_() {
  this->set_timeout("bond_wait", BOND_WAIT_MS, [this]() {
    ESP_LOGW(TAG, "Bond store: no answer from the C6");
    this->bond_finish_();
  });
}

void BTOffload::bond_next_get_() {
  if (this->bond_key_next_ >= this->bond_key_count_) {
    this->bond_write_();
    return;
  }
  const uint8_t *key = this->bond_buffer_ + this->bond_key_next_++ * BOND_KEY_SIZE;
  if (!this->nvs_get(key[0], std::string(reinterpret_cast<const char *>(key + 3), key[2]))) {
    this->bond_finish_();
    return;
  }
  this->bond_wait_();
}

void BTOffload::bond_write_() {
  this->cancel_timeout("bond_wait");
  const char *dir_rel = nullptr;
  this->bond_storage_ = storage::global_storage_registry != nullptr
                            ? storage::global_storage_registry->resolve_path(BOND_DIR, &dir_rel)
                            : nullptr;
  if (this->bond_storage_ == nullptr) {
    this->bond_finish_();
    return;
  }
  this->bond_state_ = BOND_WRITE;
  const auto queued = storage::global_storage_worker->async_mkdir(
      this->bond_storage_, dir_rel, [this](storage::StorageError) {
        const char *rel = nullptr;
        storage::PathStorage *target = storage::global_storage_registry->resolve_path(BOND_FILE, &rel);
        if (target == nullptr ||
            storage::global_storage_worker->begin_write(target, rel, &this->bond_stream_, [this](storage::StorageError err) {
              if (err != storage::StorageError::STORAGE_ERROR_OK) {
                this->bond_finish_();
                return;
              }
              auto close = [this](storage::StorageError) {
                storage::global_storage_worker->end_write(this->bond_stream_, [this](storage::StorageError done) {
                  if (done == storage::StorageError::STORAGE_ERROR_OK)
                    ESP_LOGI(TAG, "Bluetooth bonds saved to %s (%u bytes)", BOND_FILE, static_cast<unsigned>(this->bond_len_));
                  this->bond_finish_();
                });
              };
              if (this->bond_len_ == 0 ||
                  storage::global_storage_worker->write_chunk(this->bond_stream_,
                                                              this->bond_buffer_ + BOND_KEYS_MAX * BOND_KEY_SIZE,
                                                              this->bond_len_, close) != storage::StorageError::STORAGE_ERROR_OK)
                close(storage::StorageError::STORAGE_ERROR_OK);
            }) != storage::StorageError::STORAGE_ERROR_OK)
          this->bond_finish_();
      });
  if (queued != storage::StorageError::STORAGE_ERROR_OK)
    this->bond_finish_();
}

void BTOffload::bond_restore_() {
  if (this->bond_state_ != BOND_IDLE) {
    this->bond_again_ = false;
    this->bond_restore_again_ = true;
    return;
  }
  if (storage::global_storage_worker == nullptr || storage::global_storage_registry == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *target = storage::global_storage_registry->resolve_path(BOND_FILE, &rel);
  if (target == nullptr)
    return;
  this->bond_buffer_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(BOND_BUFFER);
  if (this->bond_buffer_ == nullptr)
    return;
  this->bond_state_ = BOND_READ;
  this->bond_read_ = 0;
  const auto opened = storage::global_storage_worker->begin_read(target, rel, &this->bond_stream_, [this](storage::StorageError err) {
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      this->bond_finish_();
      return;
    }
    const auto asked = storage::global_storage_worker->read_chunk(
        this->bond_stream_, this->bond_buffer_ + BOND_KEYS_MAX * BOND_KEY_SIZE, BOND_IMAGE_MAX, &this->bond_read_,
        [this](storage::StorageError read) {
          storage::global_storage_worker->end_read(this->bond_stream_, [this, read](storage::StorageError) {
            if (read != storage::StorageError::STORAGE_ERROR_OK || this->bond_read_ == 0) {
              this->bond_finish_();
              return;
            }
            this->bond_len_ = this->bond_read_;
            this->bond_at_ = 0;
            this->bond_state_ = BOND_SET;
            this->bond_next_set_();
          });
        });
    if (asked != storage::StorageError::STORAGE_ERROR_OK)
      storage::global_storage_worker->end_read(this->bond_stream_, [this](storage::StorageError) { this->bond_finish_(); });
  });
  if (opened != storage::StorageError::STORAGE_ERROR_OK)
    this->bond_finish_();
}

void BTOffload::bond_next_set_() {
  const uint8_t *image = this->bond_buffer_ + BOND_KEYS_MAX * BOND_KEY_SIZE;
  if (this->bond_at_ + 5 > this->bond_len_) {
    ESP_LOGI(TAG, "Bluetooth bonds restored to the C6 from %s", BOND_FILE);
    this->command_(TYPE_NVS_RELOAD, nullptr, 0);
    this->bond_finish_();
    return;
  }
  const uint8_t *entry = image + this->bond_at_;
  const size_t key_len = entry[2];
  const size_t value_len = entry[3 + key_len] | (entry[4 + key_len] << 8);
  if (key_len == 0 || key_len > BOND_KEY_SIZE - 3 || this->bond_at_ + 5 + key_len + value_len > this->bond_len_) {
    this->bond_finish_();
    return;
  }
  this->bond_at_ += 5 + key_len + value_len;
  if (!this->nvs_set(entry[0], std::string(reinterpret_cast<const char *>(entry + 3), key_len), entry[1],
                     entry + 5 + key_len, value_len)) {
    this->bond_finish_();
    return;
  }
  this->bond_wait_();
}

void BTOffload::bond_finish_() {
  this->cancel_timeout("bond_wait");
  if (this->bond_buffer_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->bond_buffer_, BOND_BUFFER);
    this->bond_buffer_ = nullptr;
  }
  this->bond_state_ = BOND_IDLE;
  if (this->bond_restore_again_) {
    this->bond_restore_again_ = false;
    this->bond_restore_();
    return;
  }
  if (this->bond_again_) {
    this->bond_again_ = false;
    this->bond_save_();
  }
}
#endif

void BTOffload::dump_config() { ESP_LOGCONFIG(TAG, "Bluetooth on the C6 (USB dongle through esp32_hosted)"); }

bool BTOffload::command_(uint8_t type, const uint8_t *data, size_t len) {
  if (usb_bluetooth::c6_offload::post(type, data, len))
    return true;
  ESP_LOGW(TAG, "Command 0x%02X not sent: tunnel full", type);
  return false;
}

void BTOffload::scan(uint8_t seconds) {
  ESP_LOGI(TAG, "Scan for %u s", seconds);
  if (!this->active_)
    ESP_LOGW(TAG, "Bluetooth on the C6 is not up");
  this->found_.clear();
  this->publish_scanned_();
  this->command_(TYPE_SCAN, &seconds, 1);
}

bool BTOffload::connect(const std::string &address) {
  uint8_t bda[6];
  if (!parse_address(address, bda)) {
    ESP_LOGW(TAG, "%s is no Bluetooth address", address.c_str());
    return false;
  }
  uint8_t msg[8];
  std::memcpy(msg, bda, 6);
  for (const auto &found : this->found_) {
    if (std::memcmp(found.address, bda, 6) != 0)
      continue;
    if (found.kind == DeviceKind::AUDIO)
      return this->audio_connect(address);
    if (found.kind == DeviceKind::BROADCAST) {
      uint8_t add[8 + RECORD_NAME];
      std::memcpy(add, bda, 6);
      add[6] = found.address_type;
      add[7] = static_cast<uint8_t>(std::min<size_t>(found.name.size(), RECORD_NAME));
      std::memcpy(add + 8, found.name.data(), add[7]);
      return this->command_(TYPE_BROADCAST_ADD, add, 8u + add[7]);
    }
    msg[6] = static_cast<uint8_t>(found.transport);
    msg[7] = found.address_type;
    return this->command_(TYPE_CONNECT, msg, sizeof(msg));
  }
  const Record *record = this->find_record(bda);
  if (record != nullptr && (record->kind & KIND_BROADCAST))
    return true;
  if (record != nullptr && (record->kind & KIND_AUDIO))
    return this->audio_connect(address);
  if (record != nullptr && (record->kind & KIND_HID)) {
    msg[6] = record->transport;
    msg[7] = record->addr_type;
    return this->command_(TYPE_CONNECT, msg, sizeof(msg));
  }
  ESP_LOGW(TAG, "%s is neither found nor paired", address.c_str());
  return false;
}

bool BTOffload::audio_connect(const std::string &address) {
  std::array<uint8_t, 6> bytes{};
  unsigned value[6];
  if (std::sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &value[0], &value[1], &value[2], &value[3], &value[4],
                  &value[5]) != 6) {
    ESP_LOGW(TAG, "%s is no Bluetooth address", address.c_str());
    return false;
  }
  for (size_t i = 0; i < 6; i++)
    bytes[i] = static_cast<uint8_t>(value[i]);
  this->audio_address_ = bytes;
  return this->command_(TYPE_AUDIO_CONNECT, bytes.data(), bytes.size());
}

bool BTOffload::audio_disconnect() {
  if (!this->audio_connected_)
    return false;
  return this->command_(TYPE_AUDIO_DISCONNECT, this->audio_address_.data(), this->audio_address_.size());
}

bool BTOffload::set_audio_volume(uint8_t volume) { return this->command_(TYPE_AUDIO_VOLUME, &volume, 1); }

bool BTOffload::request_status() {
  this->status_brief_ = false;
  const uint8_t full = 1;
  return this->command_(TYPE_STATUS, &full, 1);
}

void BTOffload::report_status_() {
  this->status_brief_ = true;
  this->command_(TYPE_STATUS, nullptr, 0);
}

void BTOffload::on_status_(const uint8_t *data, size_t len) {
  uint32_t v[18]{};
  if (len < 10 * sizeof(uint32_t))
    return;
  std::memcpy(v, data, std::min<size_t>(len, sizeof(v)));
  const bool quiet = this->status_brief_;
  this->publish_status_(v);
  static constexpr size_t PARTITION_WIRE = 27;
  if (quiet || len < sizeof(v) + 1)
    return;
  const uint8_t count = data[sizeof(v)];
  size_t pos = sizeof(v) + 1;
  for (uint8_t i = 0; i < count && pos + PARTITION_WIRE <= len; i++, pos += PARTITION_WIRE) {
    const uint8_t *entry = data + pos;
    uint32_t offset;
    uint32_t size;
    std::memcpy(&offset, entry + 19, sizeof(offset));
    std::memcpy(&size, entry + 23, sizeof(size));
    ESP_LOGI(TAG, "C6 partition %.16s: type %u subtype 0x%02X at 0x%06" PRIX32 " size 0x%06" PRIX32 "%s",
             reinterpret_cast<const char *>(entry), entry[16], entry[17], offset, size,
             entry[18] != 0 ? " (running)" : "");
  }
}

static bool read_varint(const uint8_t *data, size_t len, size_t *pos, uint32_t *value) {
  uint32_t result = 0;
  for (uint8_t shift = 0; shift < 35 && *pos < len; shift += 7) {
    const uint8_t b = data[(*pos)++];
    result |= static_cast<uint32_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *value = result;
      return true;
    }
  }
  return false;
}

static bool read_name(const uint8_t *data, size_t len, size_t *pos, const char **name, uint8_t *name_len) {
  if (*pos >= len)
    return false;
  *name_len = data[(*pos)++];
  if (*pos + *name_len > len)
    return false;
  *name = reinterpret_cast<const char *>(data + *pos);
  *pos += *name_len;
  return true;
}

void BTOffload::on_status_packed_(const uint8_t *data, size_t len) {
  uint32_t v[19]{};
  size_t pos = 0;
  size_t fields = 0;
  while (fields < 19 && read_varint(data, len, &pos, &v[fields]))
    fields++;
  if (fields < 18)
    return;
  this->publish_status_(v);
  if (fields >= 19 && v[18] != this->c6_log_wanted_) {
    const uint8_t log_set[2] = {this->c6_log_wanted_, this->c6_trace_wanted_};
    this->command_(TYPE_C6_LOG_SET, log_set, sizeof(log_set));
  }
}

void BTOffload::on_report_(const uint8_t *data, size_t len) {
  if (len < 1)
    return;
  size_t pos = 1;
  const char *name = nullptr;
  uint8_t name_len = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  char line[256];
  int at = 0;
  switch (data[0]) {
    case REPORT_STACKS:
    case REPORT_HEAP_OWNERS: {
      if (len < 2)
        return;
      const uint8_t count = data[pos++];
      at = snprintf(line, sizeof(line), "%s", data[0] == REPORT_STACKS ? "stack free:" : "heap by task:");
      for (uint8_t i = 0; i < count; i++) {
        if (!read_name(data, len, &pos, &name, &name_len) || !read_varint(data, len, &pos, &a))
          break;
        if (data[0] == REPORT_HEAP_OWNERS && !read_varint(data, len, &pos, &b))
          break;
        if (at < 0 || at >= static_cast<int>(sizeof(line)) - 1) {
          ESP_LOGI(TAG, "C6 %s", line);
          at = snprintf(line, sizeof(line), "%s", "  ");
        }
        if (data[0] == REPORT_STACKS) {
          at += snprintf(line + at, sizeof(line) - at, " %.*s %" PRIu32, name_len, name, a);
        } else {
          at += snprintf(line + at, sizeof(line) - at, " %.*s %" PRIu32 "/%" PRIu32, name_len, name, a, b);
        }
      }
      ESP_LOGI(TAG, "C6 %s", line);
      return;
    }
    case REPORT_BLOCKS: {
      uint32_t got = 0;
      uint32_t small_count = 0;
      uint32_t small_bytes = 0;
      if (!read_name(data, len, &pos, &name, &name_len) || !read_varint(data, len, &pos, &a) ||
          !read_varint(data, len, &pos, &got) || !read_varint(data, len, &pos, &small_count) ||
          !read_varint(data, len, &pos, &small_bytes))
        return;
      at = snprintf(line, sizeof(line), "blocks %.*s %" PRIu32 "/%" PRIu32 ":", name_len, name, a, got);
      while (at >= 0 && at < static_cast<int>(sizeof(line)) - 12 && read_varint(data, len, &pos, &b))
        at += snprintf(line + at, sizeof(line) - at, " %" PRIu32, b);
      ESP_LOGI(TAG, "C6 %s; <256: %" PRIu32 " blocks %" PRIu32 " B", line, small_count, small_bytes);
      return;
    }
    case REPORT_MARKS: {
      uint32_t total = 0;
      if (!read_varint(data, len, &pos, &a) || !read_varint(data, len, &pos, &b) ||
          !read_varint(data, len, &pos, &total) || pos >= len)
        return;
      const uint8_t count = data[pos++];
      at = snprintf(line, sizeof(line), "dropped: from P4 %" PRIu32 ", H4 to dongle %" PRIu32
                    "; internal free at: total %" PRIu32, a, b, total);
      for (uint8_t i = 0; i < count && at >= 0 && at < static_cast<int>(sizeof(line)) - 1; i++) {
        uint32_t mark = 0;
        if (!read_name(data, len, &pos, &name, &name_len) || !read_varint(data, len, &pos, &mark))
          break;
        at += snprintf(line + at, sizeof(line) - at, " %.*s %" PRIu32, name_len, name, mark);
      }
      ESP_LOGI(TAG, "C6 %s", line);
      return;
    }
    case REPORT_PARTITIONS: {
      if (len < 2)
        return;
      const uint8_t count = data[pos++];
      for (uint8_t i = 0; i < count; i++) {
        if (!read_name(data, len, &pos, &name, &name_len) || pos + 3 > len)
          return;
        const uint8_t type = data[pos];
        const uint8_t subtype = data[pos + 1];
        const bool running = data[pos + 2] != 0;
        pos += 3;
        if (!read_varint(data, len, &pos, &a) || !read_varint(data, len, &pos, &b))
          return;
        ESP_LOGI(TAG, "C6 partition %.*s: type %u subtype 0x%02X at 0x%06" PRIX32 " size 0x%06" PRIX32 "%s",
                 name_len, name, type, subtype, a, b, running ? " (running)" : "");
      }
      return;
    }
    default:
      return;
  }
}

void BTOffload::publish_status_(const uint32_t *v) {
#ifdef USE_BT_OFFLOAD_SENSOR
  for (uint8_t field = 0; field < STATUS_FIELDS; field++) {
    sensor::Sensor *sensor = this->status_sensors_[field];
    if (sensor == nullptr || field == 15)
      continue;
    float value;
    if (field == 0) {
      value = static_cast<float>(v[0]) / 1000.0f;
    } else if (field == 8 || field == 9 || field == 12) {
      value = static_cast<float>(v[field]) / 1000.0f;
    } else if (field == 16) {
      value = static_cast<float>(v[16] & 0xFFFF);
    } else if (field == 18) {
      value = static_cast<float>(v[16] >> 16);
    } else {
      value = static_cast<float>(v[field]);
    }
    sensor->publish_state(value);
  }
  if (this->status_quiet_) {
    this->status_quiet_ = false;
    return;
  }
#endif
  ESP_LOGI(TAG, "C6: %s, %s, host %s, dongle %s, crashed starts %" PRIu32 ", waiting messages %" PRIu32 " H4 %" PRIu32,
           (v[15] & C6_STATE_SAFE) != 0 ? "SAFE MODE" : "normal",
           (v[15] & C6_STATE_DONGLE) != 0 ? "on the dongle" : "on its controller",
           (v[15] & C6_STATE_READY) != 0 ? "up" : "not up",
           (v[15] & C6_STATE_REQUESTED) != 0 ? "requested" : "not requested", v[17], v[16] & 0xFFFF, v[16] >> 16);
  ESP_LOGI(TAG,
           "C6: up %" PRIu32 " ms, internal free %" PRIu32 " (min %" PRIu32 ", largest %" PRIu32 "), %" PRIu32
           " tasks, worker stack free %" PRIu32 ", audio ring free %" PRIu32 ", A2DP %" PRIu32 " calls max %" PRIu32
           " us, command max %" PRIu32 " us, frames in %" PRIu32 " out %" PRIu32 ", send max %" PRIu32
           " us, A2DP underruns %" PRIu32 ", reset reason %" PRIu32,
           v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14]);
}

void BTOffload::on_audio_state_(const uint8_t *data, size_t len) {
  if (len < 7)
    return;
  std::memcpy(this->audio_address_.data(), data, 6);
  const bool was = this->audio_connected_;
  this->audio_connected_ = data[6] != 0;
  this->audio_streaming_ = len >= 8 && data[7] != 0;
  this->audio_paging_ = len >= 9 && data[8] != 0;
  if (was != this->audio_connected_) {
    ESP_LOGI(TAG, "Audio %s %s", format_address(data).c_str(), this->audio_connected_ ? "connected" : "disconnected");
    if (this->audio_connected_)
      this->load_delay_();
#ifdef USE_BT_OFFLOAD_SPEAKER
    if (this->speaker_ != nullptr)
      this->speaker_->on_audio_state(this->audio_connected_);
#endif
  }
  this->devices_cb_.call();
}

void BTOffload::on_found_(const uint8_t *data, size_t len) {
  if (len < 9 || len < 9u + data[8])
    return;
  for (auto &found : this->found_) {
    if (std::memcmp(found.address, data, 6) != 0)
      continue;
    if (found.name.empty() && data[8] > 0) {
      found.name.assign(reinterpret_cast<const char *>(data + 9), data[8]);
      ESP_LOGI(TAG, "Found %s %s (%s)", format_address(data).c_str(), found.name.c_str(),
               found.transport == Transport::LE ? "LE" : "BR/EDR");
      this->publish_scanned_();
    }
    return;
  }
  if (this->found_.size() >= MAX_SCANNED)
    return;
  ScannedDevice found{};
  std::memcpy(found.address, data, 6);
  found.transport = data[6] == 1 ? Transport::LE : Transport::CLASSIC;
  found.address_type = data[7];
  found.name.assign(reinterpret_cast<const char *>(data + 9), data[8]);
  if (len >= 13u + data[8])
    std::memcpy(&found.device_class, data + 9 + data[8], sizeof(found.device_class));
  found.kind = found.transport == Transport::CLASSIC && ((found.device_class >> 8) & 0x1F) == 0x04 ? DeviceKind::AUDIO
                                                                                                    : DeviceKind::HID;
  if (len >= 14u + data[8] && data[13 + data[8]] == FOUND_BROADCAST)
    found.kind = DeviceKind::BROADCAST;
  ESP_LOGI(TAG, "Found %s %s (%s)", format_address(data).c_str(), found.name.c_str(),
           found.transport == Transport::LE ? "LE" : "BR/EDR");
  this->found_.push_back(std::move(found));
  this->publish_scanned_();
}

void BTOffload::publish_scanned_() {
#ifdef USE_BT_PAIRING_SELECT
  if (this->device_select_ != nullptr)
    this->device_select_->set_devices(this->found_);
#endif
  this->scanned_cb_.call();
}

void BTOffload::publish_delay_() {
#ifdef USE_BT_OFFLOAD_NUMBER
  if (this->delay_number_ != nullptr)
    this->delay_number_->publish_state(static_cast<float>(this->delay_ms_));
#endif
}

void BTOffload::pair_with(size_t index) {
  if (index < this->found_.size())
    this->connect(format_address(this->found_[index].address));
}

bool BTOffload::forget_address(const uint8_t *address) {
#ifdef USE_BT_OFFLOAD_BOND_STORE
  this->bond_forget_ = true;
#endif
  return this->command_(TYPE_FORGET, address, 6);
}

uint8_t BTOffload::record_kinds(const Record &record) const {
  const uint8_t declared = bt_host::device_class_kinds(record.transport, record.device_class);
  const OffloadSlot *slot = this->slot_(record.address);
  const uint8_t kinds = slot != nullptr ? slot->get_device_kinds() : 0;
  if (kinds == 0)
    return declared;
  if (declared == hid::HID_DEVICE_REMOTE)
    return (kinds & ~hid::HID_DEVICE_KEYBOARD) | hid::HID_DEVICE_REMOTE;
  return kinds;
}

void BTOffload::on_open_(const uint8_t *data, size_t len) {
  if (len < 7)
    return;
  const size_t name_len = data[6];
  if (len < 7 + name_len + 2)
    return;
  const size_t map_len = data[7 + name_len] | (data[8 + name_len] << 8);
  const uint8_t *map = data + 9 + name_len;
  if (len < 9 + name_len + map_len)
    return;
  OffloadSlot *slot = this->slot_(data);
  if (slot == nullptr) {
    for (auto *free_slot : this->slots_) {
      if (!free_slot->connected) {
        slot = free_slot;
        break;
      }
    }
  }
  if (slot == nullptr) {
    ESP_LOGW(TAG, "%s: no free HID slot (max_hid_devices %u)", format_address(data).c_str(), this->max_hid_devices_);
    return;
  }
  std::memcpy(slot->address, data, 6);
  if (name_len > 0) {
    const size_t len = std::min<size_t>(name_len, sizeof(slot->label) - 1);
    std::memcpy(slot->label, data + 7, len);
    slot->label[len] = '\0';
  } else {
    snprintf(slot->label, sizeof(slot->label), "%s", format_address(data).c_str());
  }
  slot->map.reset(map_len > 0 ? hid::HIDReportMap::parse_report_map_data(map, map_len) : nullptr);
  slot->connected = true;
  if (len >= 9 + name_len + map_len + 5) {
    const uint8_t *tail = map + map_len;
    slot->pad_attach(tail[4] == 1 ? hid::HIDBus::BLE : hid::HIDBus::BT_CLASSIC,
                     static_cast<uint16_t>(tail[0] | (tail[1] << 8)), static_cast<uint16_t>(tail[2] | (tail[3] << 8)));
  }
  ESP_LOGI(TAG, "%s %.*s connected (report map %u bytes)", format_address(data).c_str(), static_cast<int>(name_len),
           reinterpret_cast<const char *>(data + 7), static_cast<unsigned>(map_len));
  this->devices_cb_.call();
}

void BTOffload::on_input_(const uint8_t *data, size_t len) {
  if (len < 8)
    return;
  OffloadSlot *slot = this->slot_(data);
  if (slot == nullptr)
    return;
  const bool numbered = slot->map != nullptr && slot->map->uses_report_ids();
  uint8_t *report = const_cast<uint8_t *>(data);
  const uint8_t report_id = report[6];
  report[7] = report_id;
  if (slot->pad_report(numbered ? report + 7 : report + 8, numbered ? len - 7 : len - 8))
    return;
  if (slot->map == nullptr)
    return;
  hid::HIDReportSource source;
  if (slot->map->uses_report_ids()) {
    source.has_report_id = true;
    source.report_id = data[6];
  }
  slot->process_hid_values(slot->map->parse(source, data + 8, len - 8));
}

void BTOffload::on_close_(const uint8_t *data, size_t len) {
  if (len < 6)
    return;
  OffloadSlot *slot = this->slot_(data);
  if (slot == nullptr)
    return;
  ESP_LOGI(TAG, "%s disconnected", format_address(data).c_str());
  this->report_status_();
  slot->connected = false;
  slot->pad_detach();
  slot->release_all_hid();
  slot->map.reset();
  this->devices_cb_.call();
}

}

#endif
