#pragma once

#if defined(USE_ESP32_VARIANT_ESP32H2) || defined(USE_ESP32_VARIANT_ESP32P4)

#include "esphome/core/component.h"
#include "esphome/components/sha256/sha256.h"
#include "esphome/components/update/update_entity.h"
#include <array>
#include <memory>
#include <string>

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
#include "esphome/components/http_request/http_request.h"
#endif
#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
#include "esphome/components/network/network_component.h"
#endif

#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/storage/text_doc.h"
#endif

#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/core/static_task.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifdef USE_ESP32_HOSTED_SERIAL
#include "c6_serial.h"
#endif
#ifdef USE_ESP32_HOSTED_UPDATE_SAFE_MODE
#include "esphome/components/safe_mode/safe_mode.h"
#endif

namespace esphome::esp32_hosted {

enum class UpdateMethod : uint8_t { EMBEDDED, HTTP, STORAGE };

class Esp32HostedUpdate final : public update::UpdateEntity,
                                public PollingComponent
#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
    ,
                                public network::ActiveInterfaceListener
#endif
{
 public:
  void setup() override;
#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
  void on_active_interface(network::NetworkInterface interface) override;
#endif
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void update() override;
  void perform(bool force) override;
  void check() override;
  bool is_busy() const { return this->busy_; }
#if defined(USE_ESP32_HOSTED_SERIAL) && \
    (defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE))
  void perform_serial();
#endif

  void set_method(UpdateMethod method) { this->method_ = method; }
  void set_firmware_sha256(const std::array<uint8_t, 32> &sha256) { this->firmware_sha256_ = sha256; }
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  void set_source_url(const char *url) { this->source_url_ = url; }
  void set_http_request_parent(http_request::HttpRequestComponent *parent) { this->http_request_parent_ = parent; }
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  void set_storage_path(const std::string &path);
  const std::string &get_storage_path() const { return this->storage_path_; }
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  void set_firmware_data(const uint8_t *data) { this->firmware_data_ = data; }
  void set_firmware_size(size_t size) { this->firmware_size_ = size; }
#endif
  Trigger<float> *get_progress_trigger() { return &this->progress_trigger_; }
#ifdef USE_ESP32_HOSTED_UPDATE_SAFE_MODE
  void set_safe_mode(safe_mode::SafeModeComponent *safe_mode) { this->safe_mode_ = safe_mode; }
#endif
#ifdef USE_ESP32_HOSTED_SERIAL
  void set_serial(uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin, uint32_t baud_rate) {
    this->serial_.set_config(uart_num, tx_pin, rx_pin, boot_pin, baud_rate);
  }
  void set_serial_default(bool enabled) { this->serial_enabled_ = enabled; }
  void set_serial_settings(bool enabled, uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin);
  bool is_serial_enabled() const { return this->serial_enabled_; }
  void set_serial_log(bool on);
#endif

 protected:
  struct SlotState {
    uint8_t command;
    uint8_t result;
    uint8_t running_subtype;
    uint8_t boot_subtype;
    uint8_t minimal;
    uint8_t other_valid;
    uint8_t commands;
    uint8_t table;
    uint8_t first;
    uint8_t error_low;
    uint8_t error_high;
    uint8_t revision;
  };
  void read_version_();
  void update_announce_();
  void update_start_();
  void update_pause_();
  void update_resume_(bool ok, bool back);
  static void update_progress_(void *ctx, uint8_t percent);
  void update_task_free_();
  static void slot_on_state_(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx);
  void slot_query_(bool install);
  void slot_no_answer_();
  void slot_answered_();
  bool slot_command_(uint8_t command, uint8_t arg = 0, uint32_t wait_ms = 0);
  bool hosted_restart_();
  bool slot_switch_(bool minimal);
  bool slot_write_();
  bool offload_offered_() const;
  bool stock_offered_() const;
  bool install_with_minimal_();
  void switch_reset_();
  void fetch_minimal_();
  static void slot_task_(void *arg);
  bool app_image_ok_() const;
  Trigger<float> progress_trigger_;
#ifdef USE_ESP32_HOSTED_UPDATE_SAFE_MODE
  safe_mode::SafeModeComponent *safe_mode_{nullptr};
  bool normal_after_flash_{false};
  bool await_safe_{false};
#endif
  StaticTask flash_task_;
  SlotState slot_{};
  TaskHandle_t slot_waiter_{nullptr};
  uint8_t *main_image_{nullptr};
  uint64_t main_total_{0};
  bool minimal_first_{false};
  bool slot_install_{false};
  bool slot_return_{false};
  bool hosted_up_{true};
  bool wifi_after_flash_{false};
#ifdef USE_ESP32_HOSTED_SERIAL
  struct SerialSettings {
    uint8_t enabled;
    uint8_t uart_num;
    int8_t tx_pin;
    int8_t rx_pin;
    int8_t boot_pin;
  };
  void serial_settings_load_();
  bool serial_log_start_();
  void serial_log_stop_();
  void serial_log_parked_();
  static void serial_flash_task_(void *arg);
  static void serial_log_task_(void *arg);
  C6Serial serial_;
  ESPPreferenceObject serial_pref_;
  StaticTask log_task_;
  std::atomic<bool> log_stop_{false};
  std::atomic<bool> log_parked_{false};
  bool serial_enabled_{false};
  bool serial_route_{false};
  bool serial_wanted_{false};
  bool log_on_{false};
  bool log_after_flash_{false};
  bool log_restart_{false};
#endif
  void enter_safe_();
  void safe_install_();
  void install_failed_(const LogString *what);
  void begin_install_();
  void start_ota_();
  void next_chunk_();
  void on_chunk_();
  void install_done_(bool ok);
  void finish_install_(bool ok);
  void image_free_();
  void publish_progress_();
  void ota_image_start_();
  void ota_image_step_();
  void ota_image_failed_(const LogString *what);
  void ota_finish_();

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  bool parse_manifest_(const std::string &json_str);
  void finish_check_();
  std::string firmware_url_;
  std::string factory_url_;
  std::array<uint8_t, 32> factory_sha256_{};
  bool factory_known_{false};
  std::string minimal_url_;
  std::string main_url_;
  std::array<uint8_t, 32> minimal_sha256_{};
  std::array<uint8_t, 32> main_sha256_{};
  bool minimal_known_{false};
  uint8_t minimal_revision_{0};
  bool fallback_{false};
#endif
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  void fetch_manifest_();
  void manifest_step_();
  void manifest_failed_(const LogString *what);
  bool open_download_();
  void read_download_();
  http_request::HttpRequestComponent *http_request_parent_{nullptr};
  const char *source_url_{nullptr};
  std::shared_ptr<http_request::HttpContainer> container_;
  uint32_t last_data_time_{0};
  std::string manifest_json_;
  uint8_t initial_check_remaining_{0};
  bool initial_check_pending_{false};
  bool checking_{false};
  bool manifest_reading_{false};
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  bool is_manifest_() const;
  storage::PathStorage *open_storage_(const std::string &path, const char **rel, uint64_t *size);
  void close_stream_();
  void storage_check_();
  void storage_check_done_(bool ok, const char *why);
  bool open_storage_read_();
  void read_storage_();
  std::string storage_path_;
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  bool install_after_check_{false};
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  const uint8_t *firmware_data_{nullptr};
  size_t firmware_size_{0};
#endif

  UpdateMethod method_{UpdateMethod::EMBEDDED};
  std::array<uint8_t, 32> firmware_sha256_{};
  sha256::SHA256 hasher_;
  uint8_t *chunk_{nullptr};
  uint8_t *image_{nullptr};
  uint64_t total_{0};
  uint64_t offset_{0};
  size_t got_{0};
  uint32_t last_progress_{0};
  update::UpdateState prev_state_{update::UPDATE_STATE_UNKNOWN};
  bool busy_{false};
  bool force_{false};
};

}  // namespace esphome::esp32_hosted

#endif
