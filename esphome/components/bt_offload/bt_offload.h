#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/helpers.h"
#include "esphome/core/lock_free_queue.h"
#include "esphome/core/preferences.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"
#include "esphome/components/bt_host/bt_host.h"
#ifdef USE_BT_OFFLOAD_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER)
#define USE_BT_OFFLOAD_BOND_STORE
#include "esphome/components/storage/storage_worker.h"
#endif

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef USE_BT_OFFLOAD_PACKAGE_MANAGER
namespace esphome::package_manager {
class PackageManager;
}
#endif

namespace esphome::bt_offload {

class BTOffloadSpeaker;
#ifdef USE_BT_OFFLOAD_NUMBER
class BTOffloadDelayNumber;
#endif

using bt_host::DeviceKind;
using bt_host::ScannedDevice;
using bt_host::Transport;

struct OffloadEvent {
  static constexpr size_t INLINE_MAX = 80;
  uint32_t msg_id{0};
  uint16_t len{0};
  uint8_t inline_data[INLINE_MAX];
  uint8_t *large{nullptr};
  const uint8_t *data() const { return this->large != nullptr ? this->large : this->inline_data; }
  void release();
};

class BTOffload;

class OffloadSlot : public hid::HIDInput {
 public:
  uint8_t get_device_kinds() const override {
    return this->connected && this->map != nullptr ? this->map->device_kinds() : 0;
  }
  void set_parent(BTOffload *parent) { this->parent_ = parent; }
  bool send_output_report(const uint8_t *data, size_t len) override;
  bool set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) override;
  bool request_feature_report(uint8_t id, uint16_t len) override;
  Component *hid_component() override;
  void pad_attach(hid::HIDBus bus, uint16_t vid, uint16_t pid) {
    this->gamepad_attach_(bus, vid, pid, 0);
    this->gamepad_start_();
  }
  void pad_detach() {
    this->feature_id_ = 0;
    this->gamepad_detach_();
  }
  bool pad_report(const uint8_t *data, size_t len) { return this->gamepad_report_(data, len); }
  void pad_feature(const uint8_t *data, size_t len) {
    const uint8_t id = this->feature_id_;
    this->feature_id_ = 0;
    this->gamepad_feature_(id, data, len);
  }
  uint8_t address[6]{};
  bool connected{false};
  char label[33]{};
  std::unique_ptr<hid::HIDReportMap> map;

 protected:
  const char *device_label_() const override { return this->label; }
  BTOffload *parent_{nullptr};
  uint8_t feature_id_{0};
};

class BTOffload : public Component, public hid::HIDInput, public bt_host::DevicePicker {
 public:
  static constexpr uint8_t EVENT_SLOTS = 12;

  using Found = ScannedDevice;
  static constexpr size_t MAX_SCANNED = 16;

  static constexpr size_t RECORDS_MAX = 16;
  static constexpr size_t RECORD_NAME = 32;
  static constexpr uint8_t KIND_HID = 0x01;
  static constexpr uint8_t KIND_AUDIO = 0x02;
  static constexpr uint8_t KIND_BROADCAST = 0x04;
  static constexpr uint8_t SPACE_RECORDS = 0;
  static constexpr uint8_t SPACE_BLUEDROID = 1;

  struct Record {
    uint8_t address[6];
    uint8_t transport;
    uint8_t addr_type;
    uint8_t kind;
    uint8_t slot;
    char name[RECORD_NAME + 1];
    uint32_t device_class;
  };
  struct Records {
    const Record *first;
    const Record *last;
    const Record *begin() const { return this->first; }
    const Record *end() const { return this->last; }
    size_t size() const { return static_cast<size_t>(this->last - this->first); }
    bool empty() const { return this->first == this->last; }
    const Record &operator[](size_t index) const { return this->first[index]; }
  };
  Records records() const {
    return {this->records_, this->records_ != nullptr ? this->records_ + this->record_count_ : nullptr};
  }

  static constexpr uint32_t MAX_OUTPUT_DELAY_MS = 1600;
  bool is_connected() const { return this->audio_connected_; }
  bool is_streaming() const { return this->audio_streaming_; }
  bool is_paging() const { return this->audio_paging_; }
  bool has_stored_device() const { return this->audio_record() != nullptr; }
  uint8_t pairing_slot() const {
    const Record *record = this->audio_record();
    return record != nullptr ? record->slot : 0;
  }
  uint32_t get_paging_interval() const { return this->paging_interval_; }
  void set_paging_interval(uint32_t seconds) { this->set_audio_paging_interval(static_cast<uint16_t>(seconds)); }
  void set_paging(bool paging) { this->set_audio_paused(!paging); }
  void set_reconnect_paused(bool paused) { this->set_audio_paused(paused); }
  static constexpr int32_t MIN_OUTPUT_DELAY_MS = -800;
  int32_t output_delay_ms() const { return this->audio_connected_ ? this->delay_ms_ : 0; }
  uint32_t max_output_delay_ms() const { return MAX_OUTPUT_DELAY_MS; }
  int32_t min_output_delay_ms() const { return MIN_OUTPUT_DELAY_MS; }
  void set_output_delay_ms(int32_t ms);

  void set_max_hid_devices(uint8_t count) { this->max_hid_devices_ = count; }

  size_t record_count() const { return this->record_count_; }
  const Record *record(size_t index) const {
    return index < this->record_count_ && this->records_ != nullptr ? &this->records_[index] : nullptr;
  }
  const Record *find_record(const uint8_t *address) const;
  const Record *audio_record() const;
  bool forget(const std::string &address);
  bool forget_address(const std::string &address) { return this->forget(address); }
  bool forget_address(const uint8_t *address);
  uint8_t record_kinds(const Record &record) const;
  bool is_active() const { return this->active_; }
  void add_on_records_callback(std::function<void()> &&callback) { this->records_cb_.add(std::move(callback)); }

  bool nvs_list(uint8_t space);
  bool nvs_get(uint8_t space, const std::string &key);
  bool nvs_set(uint8_t space, const std::string &key, uint8_t type, const uint8_t *data, size_t len);
  bool nvs_erase(uint8_t space, const std::string &key);
  void add_on_nvs_entry_callback(std::function<void(uint8_t, uint8_t, const std::string &)> &&callback) {
    this->nvs_entry_cb_.add(std::move(callback));
  }
  void add_on_nvs_value_callback(
      std::function<void(uint8_t, uint8_t, const std::string &, const uint8_t *, size_t)> &&callback) {
    this->nvs_value_cb_.add(std::move(callback));
  }
  void add_on_nvs_done_callback(std::function<void(uint8_t, int32_t)> &&callback) {
    this->nvs_done_cb_.add(std::move(callback));
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void scan(uint8_t seconds);
  bool cancel_scan();
  bool is_scanning() const { return this->scanning_; }
  const std::vector<ScannedDevice> &scanned_devices() const { return this->found_; }
  const std::vector<ScannedDevice> &found() const { return this->found_; }
  void pair_with(size_t index) override;
#ifdef USE_BT_PAIRING_SELECT
  void set_device_select(bt_host::BTHostSelect *select) { this->device_select_ = select; }
#endif
#ifdef USE_BT_OFFLOAD_NUMBER
  void set_delay_number(BTOffloadDelayNumber *number) { this->delay_number_ = number; }
#endif
  void add_on_scanned_callback(std::function<void()> &&callback) { this->scanned_cb_.add(std::move(callback)); }
  void add_on_status_callback(std::function<void(const std::string &)> &&callback) {
    this->status_cb_.add(std::move(callback));
  }
  void add_on_connected_callback(std::function<void(const std::string &)> &&callback) {
    this->connected_cb_.add(std::move(callback));
  }
  void add_on_manufacturer_data_callback(
      std::function<void(const std::vector<uint8_t> &, uint16_t, const std::string &)> &&callback) {
    this->manufacturer_cb_.add(std::move(callback));
  }
  void add_on_service_data_callback(
      std::function<void(const std::vector<uint8_t> &, uint16_t, const std::string &)> &&callback) {
    this->service_cb_.add(std::move(callback));
  }
  const char *status_text() const { return this->status_text_; }
  static constexpr size_t PIN_MAX = 16;
  void set_pin(const char *pin) { snprintf(this->preset_pin_, sizeof(this->preset_pin_), "%s", pin); }
#ifdef USE_BT_OFFLOAD_PACKAGE_MANAGER
  void set_package_manager(package_manager::PackageManager *package_manager) {
    this->package_manager_ = package_manager;
  }
#endif
  const char *preset_pin() const { return this->preset_pin_; }
  void set_preset_pin(const std::string &pin);
  bool pair_reply(const std::string &code);
  bool pair_cancel();
  void add_on_code_request_callback(std::function<void(const std::string &, bool, const std::string &)> &&callback) {
    this->has_code_ui_ = true;
    this->code_cb_.add(std::move(callback));
  }
  bool connect(const std::string &address);
  bool pair_with(const std::string &address) { return this->connect(address); }
  bool disconnect(const std::string &address);
  bool is_connected(const std::string &address) const;

  uint8_t get_device_kinds() const override;
  void list_devices(std::vector<hid::InputDevice> &out) const override;
  hid::HIDInput *device_input(const std::string &key) override;
  void set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) override;

  bool audio_connect(const std::string &address);
  bool audio_disconnect();
  bool set_audio_volume(uint8_t volume);
  bool set_audio_paused(bool paused);
  bool set_audio_paging_interval(uint16_t seconds);
  uint16_t audio_paging_interval() const { return this->paging_interval_; }
  bool request_status();
  void report_status_();
  bool status_brief_{false};
#ifdef USE_BT_OFFLOAD_SENSOR
  static constexpr uint8_t STATUS_FIELDS = 19;
  void set_status_sensor(uint8_t field, sensor::Sensor *sensor) {
    if (field < STATUS_FIELDS)
      this->status_sensors_[field] = sensor;
  }
  void set_status_interval(uint32_t ms) { this->status_interval_ = ms; }
#endif
  bool hid_report(uint8_t type, const uint8_t *address, int prefix, const uint8_t *data, size_t len);
  bool set_c6_log(uint8_t level);
  void set_c6_log_level(uint8_t level) { this->c6_log_override_ = level; }
  bool is_audio_connected() const { return this->audio_connected_; }
  bool is_audio_streaming() const { return this->audio_streaming_; }
  bool is_audio_paging() const { return this->audio_paging_; }
  bool has_audio_device() const { return this->audio_record() != nullptr; }
  void set_speaker(BTOffloadSpeaker *speaker) { this->speaker_ = speaker; }

  void on_open_(const uint8_t *data, size_t len);
  void on_input_(const uint8_t *data, size_t len);
  void on_close_(const uint8_t *data, size_t len);
  void on_feature_(const uint8_t *data, size_t len);
  void on_input_raw_(const uint8_t *data, size_t len);
  void on_found_(const uint8_t *data, size_t len);
  void on_scan_state_(const uint8_t *data, size_t len);
  void on_pair_event_(const uint8_t *data, size_t len);
  void on_code_request_(const uint8_t *data, const std::string &name);
  void on_audio_state_(const uint8_t *data, size_t len);
  void on_status_(const uint8_t *data, size_t len);
  void on_status_packed_(const uint8_t *data, size_t len);
  void publish_status_(const uint32_t *v);
  void on_report_(const uint8_t *data, size_t len);
  void on_records_(const uint8_t *data, size_t len);
  void on_nvs_entry_(const uint8_t *data, size_t len);
  void on_nvs_value_(const uint8_t *data, size_t len);
  void on_nvs_done_(const uint8_t *data, size_t len);
  void on_broadcast_(const uint8_t *data, size_t len);
  void receive_(uint32_t msg_id, const uint8_t *data, size_t len);

 protected:
  struct DeviceLayout {
    uint8_t address[6];
    const hid::KeyboardLayout *layout;
  };
  OffloadSlot *slot_(const uint8_t *address) const;
  const hid::KeyboardLayout *layout_of_(const uint8_t *address) const;
  void add_slot_(OffloadSlot *slot);
  bool command_(uint8_t type, const uint8_t *data, size_t len);
  bool send_pair_reply_(bool accept, const std::string &code);
  bool address_command_(uint8_t type, const std::string &address);
  bool nvs_key_command_(uint8_t type, uint8_t space, const std::string &key, uint8_t value_type, const uint8_t *data,
                        size_t len);
#ifdef USE_BT_OFFLOAD_BOND_STORE
  static constexpr size_t BOND_KEYS_MAX = 48;
  static constexpr size_t BOND_KEY_SIZE = 3 + 15;
  static constexpr size_t BOND_IMAGE_MAX = 8192;
  static constexpr size_t BOND_BUFFER = BOND_KEYS_MAX * BOND_KEY_SIZE + BOND_IMAGE_MAX;
  static constexpr uint32_t BOND_WAIT_MS = 5000;
  void bond_wait_();
  void bond_save_();
  void bond_restore_();
  void bond_next_get_();
  void bond_write_();
  void bond_next_set_();
  void bond_finish_();
  uint32_t bond_signature_() const;
  uint8_t *bond_buffer_{nullptr};
  size_t bond_len_{0};
  size_t bond_at_{0};
  size_t bond_key_count_{0};
  size_t bond_key_next_{0};
  uint8_t bond_state_{0};
  bool bond_again_{false};
  bool bond_restore_again_{false};
  bool bond_forget_{false};
  uint32_t bond_sig_{0};
  size_t bond_read_{0};
  storage::PathStorage *bond_storage_{nullptr};
  storage::StreamHandle bond_stream_{};
#endif

  uint8_t max_hid_devices_{4};
  std::vector<OffloadSlot *> slots_;
  std::vector<DeviceLayout> layouts_;
  Record *records_{nullptr};
  size_t record_count_{0};
  CallbackManager<void()> records_cb_;
  CallbackManager<void()> scanned_cb_;
  CallbackManager<void(const std::string &)> status_cb_;
  CallbackManager<void(const std::string &)> connected_cb_;
  CallbackManager<void(const std::vector<uint8_t> &, uint16_t, const std::string &)> manufacturer_cb_;
  CallbackManager<void(const std::vector<uint8_t> &, uint16_t, const std::string &)> service_cb_;
  char status_text_[96]{};
  CallbackManager<void(const std::string &, bool, const std::string &)> code_cb_;
  ESPPreferenceObject pin_pref_;
#ifdef USE_BT_OFFLOAD_PACKAGE_MANAGER
  package_manager::PackageManager *package_manager_{nullptr};
#endif
  char preset_pin_[PIN_MAX + 1]{};
  uint8_t pending_address_[6]{};
  uint8_t pending_kind_{0};
  bool has_code_ui_{false};
  CallbackManager<void(uint8_t, uint8_t, const std::string &)> nvs_entry_cb_;
  CallbackManager<void(uint8_t, uint8_t, const std::string &, const uint8_t *, size_t)> nvs_value_cb_;
  CallbackManager<void(uint8_t, int32_t)> nvs_done_cb_;

  void publish_scanned_();
  void publish_delay_();
  std::vector<ScannedDevice> found_;
#ifdef USE_BT_PAIRING_SELECT
  bt_host::BTHostSelect *device_select_{nullptr};
#endif
#ifdef USE_BT_OFFLOAD_SENSOR
  std::array<sensor::Sensor *, STATUS_FIELDS> status_sensors_{};
  uint32_t status_interval_{0};
  bool status_quiet_{false};
#endif
  uint8_t c6_log_override_{0xFF};
  uint8_t c6_log_wanted_{0};
  uint8_t c6_trace_wanted_{0};
#ifdef USE_BT_OFFLOAD_NUMBER
  BTOffloadDelayNumber *delay_number_{nullptr};
#endif
  bool active_{false};
  bool scanning_{false};
  BTOffloadSpeaker *volatile speaker_{nullptr};
  std::array<uint8_t, 6> audio_address_{};
  bool audio_connected_{false};
  bool audio_streaming_{false};
  bool audio_paging_{false};
  bool audio_paused_{false};
  uint16_t paging_interval_{10};
  int32_t delay_ms_{0};
  void load_delay_();
  EventPool<OffloadEvent, EVENT_SLOTS> pool_;
  LockFreeQueue<OffloadEvent, EVENT_SLOTS + 1> queue_;
};

class StatusTrigger : public Trigger<std::string> {
 public:
  explicit StatusTrigger(BTOffload *parent) {
    parent->add_on_status_callback([this](const std::string &message) { this->trigger(message); });
  }
};

class CodeRequestTrigger : public Trigger<std::string, bool, std::string> {
 public:
  explicit CodeRequestTrigger(BTOffload *parent) {
    parent->add_on_code_request_callback([this](const std::string &device, bool pin, const std::string &preset) {
      this->trigger(device, pin, preset);
    });
  }
};

template<typename... Ts> class PairReplyAction : public Action<Ts...> {
 public:
  explicit PairReplyAction(BTOffload *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, code)
  void play(const Ts &...x) override { this->parent_->pair_reply(this->code_.value(x...)); }

 protected:
  BTOffload *parent_;
};

template<typename... Ts> class PairCancelAction : public Action<Ts...> {
 public:
  explicit PairCancelAction(BTOffload *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->pair_cancel(); }

 protected:
  BTOffload *parent_;
};

class ConnectedTrigger : public Trigger<std::string> {
 public:
  explicit ConnectedTrigger(BTOffload *parent) {
    parent->add_on_connected_callback([this](const std::string &device) { this->trigger(device); });
  }
};

class ManufacturerDataTrigger : public Trigger<std::vector<uint8_t>, uint16_t, std::string> {
 public:
  explicit ManufacturerDataTrigger(BTOffload *parent) {
    parent->add_on_manufacturer_data_callback(
        [this](const std::vector<uint8_t> &data, uint16_t id, const std::string &device) {
          this->trigger(data, id, device);
        });
  }
};

class ServiceDataTrigger : public Trigger<std::vector<uint8_t>, uint16_t, std::string> {
 public:
  explicit ServiceDataTrigger(BTOffload *parent) {
    parent->add_on_service_data_callback(
        [this](const std::vector<uint8_t> &data, uint16_t uuid, const std::string &device) {
          this->trigger(data, uuid, device);
        });
  }
};

}

#endif
