#if defined(USE_ESP32_VARIANT_ESP32H2) || defined(USE_ESP32_VARIANT_ESP32P4)
#include "esp32_hosted_update.h"
#include "esphome/components/esp32/system_info.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <esp_image_format.h>
#include <esp_app_desc.h>
#include <esp_hosted.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_ota_ops.h>
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
#include "esphome/components/network/util.h"
#endif
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
#include "esphome/components/json/json_util.h"
#endif

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#include <esp_netif.h>
#include <esp_wifi.h>
#endif
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_USB_ETHERNET
#include "esphome/components/usb_ethernet/usb_ethernet.h"
#endif
#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD
#include "esphome/components/usb_bluetooth/c6_offload.h"
#endif

extern "C" {
#include <esp_hosted_ota.h>
}

namespace esphome::esp32_hosted {

ESPHOME_LOG_TAG(TAG, "esp32_hosted.update");

// The host rejects writes above its per-RPC payload limit before they reach the coprocessor
constexpr size_t STOCK_SDIO_PACKET = 1536;
constexpr size_t CHUNK_SIZE = EH_HOST_CP_OTA_CHUNK_MAX - (ESP_TRANSPORT_SDIO_MAX_BUF_SIZE - STOCK_SDIO_PACKET);
// OTA begin blocks while the coprocessor erases its partition, end while it validates the image
constexpr uint32_t OTA_WDT_TIMEOUT_MS = 60000;
constexpr uint32_t PROGRESS_MS = 500;
constexpr uint32_t SLOT_MSG_COMMAND = 0x42545070;
constexpr uint32_t SLOT_MSG_STATE = 0x42545071;
constexpr uint8_t SLOT_QUERY = 0;
constexpr uint8_t SLOT_BOOT_OTHER = 1;
constexpr uint8_t SLOT_BOOT_MAIN = 2;
constexpr uint8_t SLOT_BOOT_MINIMAL = 3;
constexpr uint8_t SLOT_CLONE = 4;
constexpr uint8_t SLOT_TABLE = 5;
constexpr uint8_t SLOT_TABLE_OURS = 1;
constexpr uint8_t SLOT_TABLE_STOCK = 2;
constexpr uint64_t STOCK_APP_SIZE = 0x1C0000;
constexpr uint32_t SLOT_CLONE_MS = 120000;
constexpr uint32_t SLOT_TABLE_MS = 30000;
constexpr size_t SLOT_STATE_MIN = 6;
constexpr uint32_t SLOT_ANSWER_MS = 3000;
constexpr uint32_t SLOT_TIMEOUT_ID = 1;
constexpr uint32_t UPDATE_TASK_STACK = 6144;
constexpr UBaseType_t UPDATE_TASK_PRIORITY = 5;
constexpr uint32_t UPDATE_FREE_RETRY_MS = 10;
constexpr uint32_t UPDATE_NOTICE_MS = 3000;
constexpr uint8_t SLOT_PROGRESS_STEP = 5;
#ifdef USE_ESP32_HOSTED_SERIAL
constexpr uint32_t SERIAL_LOG_STACK = 4096;
constexpr UBaseType_t SERIAL_LOG_PRIORITY = 2;
#endif

static esp_err_t ota_end_guarded() {
  watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
  return esp_hosted_slave_ota_end();  // NOLINT
}

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
// Interval/timeout IDs (uint32_t to avoid string comparison)
constexpr uint32_t INITIAL_CHECK_INTERVAL_ID = 0;
#endif

// Compile-time version string from esp_hosted_host_fw_ver.h macros
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
static const char *const ESP_HOSTED_VERSION_STR = STRINGIFY(ESP_HOSTED_VERSION_MAJOR_1) "." STRINGIFY(
    ESP_HOSTED_VERSION_MINOR_1) "." STRINGIFY(ESP_HOSTED_VERSION_PATCH_1);

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
// Parse an integer from str, advancing ptr past the number
// Returns false if no digits were parsed
static bool parse_int(const char *&ptr, int &value) {
  char *end;
  value = static_cast<int>(strtol(ptr, &end, 10));
  if (end == ptr)
    return false;
  ptr = end;
  return true;
}

// Parse version string "major.minor.patch" into components
// Returns true if at least major.minor was parsed
static bool parse_version(const std::string &version_str, int &major, int &minor, int &patch) {
  major = minor = patch = 0;
  const char *ptr = version_str.c_str();

  if (!parse_int(ptr, major) || *ptr != '.')
    return false;
  ++ptr;
  if (!parse_int(ptr, minor))
    return false;
  if (*ptr == '.')
    parse_int(++ptr, patch);

  return true;
}

// Compare two versions, returns:
//  -1 if v1 < v2
//   0 if v1 == v2
//   1 if v1 > v2
static int compare_versions(int major1, int minor1, int patch1, int major2, int minor2, int patch2) {
  if (major1 != major2)
    return major1 < major2 ? -1 : 1;
  if (minor1 != minor2)
    return minor1 < minor2 ? -1 : 1;
  if (patch1 != patch2)
    return patch1 < patch2 ? -1 : 1;
  return 0;
}
#endif

void Esp32HostedUpdate::read_version_() {
  esp_hosted_coprocessor_fwver_t ver_info;
  if (esp_hosted_get_coprocessor_fwversion(&ver_info) == ESP_OK) {
    char buf[52];
    if (ver_info.build > 0) {
      snprintf(buf, sizeof(buf), "%" PRIu32 ".%" PRIu32 ".%" PRIu32 "+xbt.%" PRId32, ver_info.major1, ver_info.minor1,
               ver_info.patch1, ver_info.build);
    } else {
      snprintf(buf, sizeof(buf), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, ver_info.major1, ver_info.minor1, ver_info.patch1);
    }
    this->update_info_.current_version = buf;
  } else {
    this->update_info_.current_version = "unknown";
  }
  ESP_LOGD(TAG, "Coprocessor version: %s", this->update_info_.current_version.c_str());
}

void Esp32HostedUpdate::setup() {
  this->update_info_.title = "ESP32 Hosted Coprocessor";

#ifndef USE_WIFI
  // If WiFi is not present, connect to the coprocessor
  esp_hosted_connect_to_slave();  // NOLINT
#endif

  this->read_version_();
#ifdef USE_ESP32_HOSTED_SERIAL
  this->serial_settings_load_();
#endif
  this->slot_query_(false);
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
  if (this->safe_mode_ != nullptr) {
    this->safe_mode_->add_on_safe_ready_callback([this]() {
      if (!this->await_safe_)
        return;
      this->await_safe_ = false;
      this->defer([this]() { this->safe_install_(); });
    });
  }
#endif

#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  if (this->method_ == UpdateMethod::EMBEDDED) {
    // Embedded mode: get image version from embedded firmware
    const int app_desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (this->firmware_size_ >= app_desc_offset + sizeof(esp_app_desc_t)) {
      esp_app_desc_t *app_desc = (esp_app_desc_t *) (this->firmware_data_ + app_desc_offset);
      if (app_desc->magic_word == ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGD(TAG,
                 "ESP32 Hosted firmware:\n"
                 "  Firmware version: %s\n"
                 "  Project name: %s\n"
                 "  Build date: %s\n"
                 "  Build time: %s\n"
                 "  IDF version: %s",
                 app_desc->version, app_desc->project_name, app_desc->date, app_desc->time, app_desc->idf_ver);
        this->update_info_.latest_version = app_desc->version;
        if (this->update_info_.latest_version != this->update_info_.current_version) {
          this->state_ = update::UPDATE_STATE_AVAILABLE;
        } else {
          this->state_ = update::UPDATE_STATE_NO_UPDATE;
        }
      } else {
        ESP_LOGW(TAG, "Invalid app description magic word: 0x%08" PRIx32 " (expected 0x%08" PRIx32 ")",
                 app_desc->magic_word, static_cast<uint32_t>(ESP_APP_DESC_MAGIC_WORD));
        this->state_ = update::UPDATE_STATE_NO_UPDATE;
      }
    } else {
      ESP_LOGW(TAG, "Firmware too small to contain app description");
      this->state_ = update::UPDATE_STATE_NO_UPDATE;
    }

    // Publish state
    this->status_clear_error();
    this->publish_state();
    return;
  }
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  if (this->method_ == UpdateMethod::STORAGE) {
    if (this->is_manifest_()) {
      this->state_ = update::UPDATE_STATE_NO_UPDATE;
      this->publish_state();
      return;
    }
    // Storage mode: no availability/version discovery — the firmware to install is whatever the
    // user placed on the configured storage path. Offer it as available; the install reads and
    // verifies (sha256) at perform() time.
    this->update_info_.latest_version = "storage";
    this->state_ = update::UPDATE_STATE_AVAILABLE;
    this->status_clear_error();
    this->publish_state();
    // Defer so the automation runs on the main loop after setup, not during App.setup()
    if (this->update_available_trigger_) {
      this->defer([this]() { this->update_available_trigger_->trigger(this->update_info_); });
    }
    return;
  }
#endif
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  if (this->method_ == UpdateMethod::HTTP) {
    // HTTP mode: check every 10s until network is ready (max 6 attempts)
    // Only if update interval is > 1 minute to avoid redundant checks
#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
    this->initial_check_pending_ = true;
    return;
#endif
    if (this->get_update_interval() > 60000) {
      this->initial_check_remaining_ = 6;
      this->set_interval(INITIAL_CHECK_INTERVAL_ID, 10000, [this]() {
        bool connected = network::is_connected();
        if (--this->initial_check_remaining_ == 0 || connected) {
          this->cancel_interval(INITIAL_CHECK_INTERVAL_ID);
          if (connected) {
            this->check();
          }
        }
      });
    }
  }
#endif
}

void Esp32HostedUpdate::update() {
#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
  if (this->initial_check_pending_)
    return;
#endif
  this->check();
}

#ifdef USE_ESP32_HOSTED_UPDATE_NETWORK_EVENT
void Esp32HostedUpdate::on_active_interface(network::NetworkInterface interface) {
  if (!this->initial_check_pending_ || interface == network::NetworkInterface::NETWORK_INTERFACE_NONE)
    return;
  this->initial_check_pending_ = false;
  this->defer([this]() { this->check(); });
}
#endif

void Esp32HostedUpdate::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32 Hosted Update:\n"
                "  Host Library Version: %s\n"
                "  Coprocessor Version: %s\n"
                "  Latest Version: %s",
                ESP_HOSTED_VERSION_STR, this->update_info_.current_version.c_str(),
                this->update_info_.latest_version.c_str());
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  if (this->method_ == UpdateMethod::HTTP)
    ESP_LOGCONFIG(TAG,
                  "  Mode: HTTP\n"
                  "  Source URL: %s",
                  this->source_url_);
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  if (this->method_ == UpdateMethod::STORAGE)
    ESP_LOGCONFIG(TAG,
                  "  Mode: Storage\n"
                  "  Path: %s",
                  this->storage_path_.c_str());
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  if (this->method_ == UpdateMethod::EMBEDDED)
    ESP_LOGCONFIG(TAG,
                  "  Mode: Embedded\n"
                  "  Firmware Size: %zu bytes",
                  this->firmware_size_);
#endif
}

void Esp32HostedUpdate::check() {
  if (this->busy_)
    return;
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  if (this->method_ == UpdateMethod::STORAGE) {
    if (this->is_manifest_())
      this->storage_check_();
    return;
  }
#endif
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  if (this->method_ == UpdateMethod::HTTP) {
    if (this->source_url_ == nullptr || this->source_url_[0] == '\0' ||
        std::strstr(this->source_url_, "//0.0.0.0") != nullptr)
      return;
    if (!network::is_connected()) {
      ESP_LOGD(TAG, "Network not connected, skipping update check");
      return;
    }
    if (this->checking_)
      return;
    this->checking_ = true;
    this->fetch_manifest_();
  }
#endif
}

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
void Esp32HostedUpdate::finish_check_() {
  const bool was_available = this->state_ == update::UPDATE_STATE_AVAILABLE;

  // Compare versions
  if (this->update_info_.latest_version.empty() ||
      this->update_info_.latest_version == this->update_info_.current_version) {
    this->state_ = update::UPDATE_STATE_NO_UPDATE;
  } else {
    this->state_ = update::UPDATE_STATE_AVAILABLE;
  }

  this->update_info_.has_progress = false;
  this->update_info_.progress = 0.0f;
  this->status_clear_error();
  this->publish_state();
  if (this->state_ == update::UPDATE_STATE_AVAILABLE && !was_available && this->update_available_trigger_) {
    this->update_available_trigger_->trigger(this->update_info_);
  }
}

bool Esp32HostedUpdate::parse_manifest_(const std::string &json_str) {
  // Parse JSON manifest
  // Format: {"versions": [{"version": "2.7.0", "url": "...", "sha256": "..."}]}
  // Only consider versions <= host library version to avoid compatibility issues
  return json::parse_json(json_str, [this](JsonObject root) -> bool {
    if (!root["versions"].is<JsonArray>()) {
      ESP_LOGE(TAG, "Manifest does not contain 'versions' array");
      return false;
    }

    JsonArray versions = root["versions"].as<JsonArray>();
    if (versions.size() == 0) {
      ESP_LOGE(TAG, "Manifest 'versions' array is empty");
      return false;
    }

    // Find the highest version that is compatible with the host library
    // (version <= host version to avoid upgrading coprocessor ahead of host)
    int best_major = -1, best_minor = -1, best_patch = -1;
    std::string best_version, best_url, best_sha256, best_factory_url, best_factory_sha256;
    std::string best_minimal_url, best_minimal_sha256;
    uint8_t best_minimal_revision = 0;

    for (JsonObject entry : versions) {
      if (!entry["version"].is<const char *>() || !entry["url"].is<const char *>() ||
          !entry["sha256"].is<const char *>()) {
        continue;  // Skip malformed entries
      }

      std::string ver_str = entry["version"].as<std::string>();
      int major, minor, patch;
      if (!parse_version(ver_str, major, minor, patch)) {
        ESP_LOGW(TAG, "Failed to parse version: %s", ver_str.c_str());
        continue;
      }

      // Check if this version is compatible (not newer than host)
      if (compare_versions(major, minor, patch, ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                           ESP_HOSTED_VERSION_PATCH_1) > 0) {
        continue;
      }

      // Check if this is better than our current best
      if (best_major < 0 || compare_versions(major, minor, patch, best_major, best_minor, best_patch) > 0) {
        best_major = major;
        best_minor = minor;
        best_patch = patch;
        best_version = ver_str;
        best_url = entry["url"].as<std::string>();
        best_sha256 = entry["sha256"].as<std::string>();
        const bool factory = entry["factory_url"].is<const char *>() && entry["factory_sha256"].is<const char *>();
        best_factory_url = factory ? entry["factory_url"].as<std::string>() : std::string();
        best_factory_sha256 = factory ? entry["factory_sha256"].as<std::string>() : std::string();
        const bool minimal = entry["minimal_url"].is<const char *>() && entry["minimal_sha256"].is<const char *>();
        best_minimal_url = minimal ? entry["minimal_url"].as<std::string>() : std::string();
        best_minimal_sha256 = minimal ? entry["minimal_sha256"].as<std::string>() : std::string();
        best_minimal_revision = 0;
        if (minimal && entry["minimal_revision"].is<int>())
          best_minimal_revision = entry["minimal_revision"].as<uint8_t>();
      }
    }

    if (best_major < 0) {
      ESP_LOGW(TAG, "No compatible firmware version found (host is %s)", ESP_HOSTED_VERSION_STR);
      return false;
    }

    this->update_info_.latest_version = best_version;
    this->firmware_url_ = best_url;

    // Parse SHA256 hex string to bytes
    if (!parse_hex(best_sha256, this->firmware_sha256_.data(), 32)) {
      ESP_LOGE(TAG, "Invalid SHA256: %s", best_sha256.c_str());
      return false;
    }
    this->factory_known_ =
        !best_factory_url.empty() && parse_hex(best_factory_sha256, this->factory_sha256_.data(), 32);
    this->factory_url_ = this->factory_known_ ? best_factory_url : std::string();
    this->minimal_known_ =
        !best_minimal_url.empty() && parse_hex(best_minimal_sha256, this->minimal_sha256_.data(), 32);
    this->minimal_url_ = this->minimal_known_ ? best_minimal_url : std::string();
    this->minimal_revision_ = this->minimal_known_ ? best_minimal_revision : 0;

    ESP_LOGD(TAG, "Best compatible version: %s", this->update_info_.latest_version.c_str());

    return true;
  });
}
#endif

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
void Esp32HostedUpdate::fetch_manifest_() {
  ESP_LOGD(TAG, "Fetching manifest");
  this->container_ = this->http_request_parent_->begin(this->source_url_, "GET", std::string(), {}, {},
                                                       this->http_request_parent_->get_timeout());
  if (this->container_ == nullptr) {
    this->manifest_failed_(LOG_STR("Failed to fetch manifest"));
    return;
  }
  this->manifest_reading_ = false;
  this->defer([this]() { this->manifest_step_(); });
}

void Esp32HostedUpdate::manifest_step_() {
  if (this->container_ == nullptr)
    return;
  if (!this->manifest_reading_) {
    const auto progress = this->container_->poll();
    if (progress == http_request::HttpRequestProgress::HTTP_REQUEST_PROGRESS_PENDING) {
      this->defer([this]() { this->manifest_step_(); });
      return;
    }
    if (progress != http_request::HttpRequestProgress::HTTP_REQUEST_PROGRESS_DONE ||
        this->container_->status_code != 200) {
      this->manifest_failed_(LOG_STR("Failed to fetch manifest"));
      return;
    }
    this->manifest_reading_ = true;
    this->manifest_json_.clear();
    this->manifest_json_.reserve(this->container_->content_length);
    this->last_data_time_ = millis();
  }
  uint8_t buf[256];
  const int read_or_error = this->container_->read_available(buf, sizeof(buf));
  const auto result = http_request::http_read_loop_result(read_or_error, this->last_data_time_,
                                                         this->http_request_parent_->get_timeout(),
                                                         this->container_->is_read_complete());
  if (result == http_request::HttpReadLoopResult::DATA)
    this->manifest_json_.append(reinterpret_cast<char *>(buf), read_or_error);
  if (result == http_request::HttpReadLoopResult::RETRY ||
      (result == http_request::HttpReadLoopResult::DATA &&
       this->container_->get_bytes_read() < this->container_->content_length)) {
    this->defer([this]() { this->manifest_step_(); });
    return;
  }
  if (result == http_request::HttpReadLoopResult::ERROR || result == http_request::HttpReadLoopResult::TIMEOUT) {
    this->manifest_failed_(LOG_STR("Failed to fetch manifest"));
    return;
  }
  this->container_->end();
  this->container_.reset();
  const bool parsed = this->parse_manifest_(this->manifest_json_);
  std::string().swap(this->manifest_json_);
  this->checking_ = false;
  if (!parsed) {
    ESP_LOGE(TAG, "Failed to parse manifest JSON");
    this->status_set_error(LOG_STR("Failed to parse manifest"));
    return;
  }
  this->finish_check_();
}

void Esp32HostedUpdate::manifest_failed_(const LogString *what) {
  ESP_LOGE(TAG, "%s from %s", LOG_STR_ARG(what), this->source_url_);
  if (this->container_ != nullptr) {
    this->container_->end();
    this->container_.reset();
  }
  std::string().swap(this->manifest_json_);
  this->checking_ = false;
  this->status_set_error(what);
}

bool Esp32HostedUpdate::open_download_() {
  ESP_LOGI(TAG, "Downloading firmware");
  this->container_ = this->http_request_parent_->get(this->firmware_url_);
  if (this->container_ == nullptr || this->container_->status_code != 200 || this->container_->content_length == 0) {
    ESP_LOGE(TAG, "Failed to fetch firmware");
    this->status_set_error(LOG_STR("Failed to fetch firmware"));
    return false;
  }
  this->total_ = this->container_->content_length;
  this->last_data_time_ = millis();
  return true;
}

void Esp32HostedUpdate::read_download_() {
  // NOTE: HttpContainer::read() has non-BSD socket semantics - see http_request.h
  const size_t want = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - this->offset_));
  const int read_or_error = this->container_->read(this->chunk_, want);
  const auto result = http_request::http_read_loop_result(read_or_error, this->last_data_time_,
                                                         this->http_request_parent_->get_timeout(),
                                                         this->container_->is_read_complete());
  if (result == http_request::HttpReadLoopResult::RETRY) {
    this->defer([this]() { this->read_download_(); });
    return;
  }
  if (result != http_request::HttpReadLoopResult::DATA) {
    ESP_LOGE(TAG, "Error reading firmware data: %d", read_or_error);
    this->status_set_error(LOG_STR("Download failed"));
    this->install_done_(false);
    return;
  }
  this->got_ = static_cast<size_t>(read_or_error);
  this->on_chunk_();
}
#endif

#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
void Esp32HostedUpdate::set_storage_path(const std::string &path) {
  if (this->busy_)
    return;
  this->storage_path_ = path;
  if (this->method_ == UpdateMethod::STORAGE && this->is_manifest_()) {
    this->firmware_url_.clear();
    this->update_info_.latest_version.clear();
    this->state_ = update::UPDATE_STATE_NO_UPDATE;
  }
}

bool Esp32HostedUpdate::is_manifest_() const {
  const size_t n = this->storage_path_.size();
  return n > 5 && strcasecmp(this->storage_path_.c_str() + n - 5, ".json") == 0;
}

storage::PathStorage *Esp32HostedUpdate::open_storage_(const std::string &path, const char **rel, uint64_t *size) {
  storage::PathStorage *ps = storage::global_storage_registry == nullptr
                                 ? nullptr
                                 : storage::global_storage_registry->resolve_path(path.c_str(), rel);
  storage::FileStat st{};
  if (ps == nullptr || *rel == nullptr || storage::global_storage_worker == nullptr ||
      storage::global_storage_worker->has_active_task_io(ps) ||
      ps->stat(*rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir || st.size == 0) {
    ESP_LOGE(TAG, "'%s' is not a readable file on a mounted storage", path.c_str());
    return nullptr;
  }
  *size = st.size;
  return ps;
}

void Esp32HostedUpdate::close_stream_() {
  if (this->stream_open_) {
    this->stream_open_ = false;
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
  }
}

void Esp32HostedUpdate::storage_check_() {
  this->busy_ = true;
  const storage::StorageError err = storage::text::load(
      this->storage_path_, [this](storage::StorageError e, const char *text, size_t len) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->storage_check_done_(false, storage::error_to_string(e));
          return;
        }
        this->storage_check_done_(this->parse_manifest_(std::string(text, len)), "no usable entry");
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->storage_check_done_(false, storage::error_to_string(err));
}

void Esp32HostedUpdate::storage_check_done_(bool ok, const char *why) {
  const bool install = this->install_after_check_;
  this->install_after_check_ = false;
  this->busy_ = false;
  if (!ok) {
    if (install) {
      ESP_LOGE(TAG, "No manifest at %s (%s)", this->storage_path_.c_str(), why);
      this->install_failed_(LOG_STR("Manifest not readable"));
      return;
    }
    ESP_LOGD(TAG, "No manifest at %s (%s)", this->storage_path_.c_str(), why);
    this->publish_state();
    return;
  }
  if (!this->firmware_url_.empty() && this->firmware_url_[0] != '/') {
    const size_t slash = this->storage_path_.rfind('/');
    this->firmware_url_.insert(0, this->storage_path_, 0, slash + 1);
  }
  if (!this->factory_url_.empty() && this->factory_url_[0] != '/') {
    const size_t slash = this->storage_path_.rfind('/');
    this->factory_url_.insert(0, this->storage_path_, 0, slash + 1);
  }
  if (!this->minimal_url_.empty() && this->minimal_url_[0] != '/') {
    const size_t slash = this->storage_path_.rfind('/');
    this->minimal_url_.insert(0, this->storage_path_, 0, slash + 1);
  }
  this->finish_check_();
  if (!install)
    return;
  bool wanted = this->force_ || this->state_ == update::UPDATE_STATE_AVAILABLE;
#ifdef USE_ESP32_HOSTED_SERIAL
  wanted = wanted || this->serial_wanted_;
#endif
  if (!wanted) {
    ESP_LOGI(TAG, "C6 is up to date");
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
    if (this->normal_after_flash_) {
      this->normal_after_flash_ = false;
      this->safe_mode_->go_normal();
    }
#endif
    return;
  }
  if (this->firmware_url_.empty()) {
    this->install_failed_(LOG_STR("No firmware in the manifest"));
    return;
  }
  this->prev_state_ = this->state_;
  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->update_info_.has_progress = false;
  this->publish_state();
  this->busy_ = true;
  this->begin_install_();
}

bool Esp32HostedUpdate::open_storage_read_() {
  const std::string &path = this->is_manifest_() ? this->firmware_url_ : this->storage_path_;
  ESP_LOGI(TAG, "Reading firmware from '%s'", path.c_str());
  const char *rel = nullptr;
  storage::PathStorage *ps = this->open_storage_(path, &rel, &this->total_);
  if (ps == nullptr) {
    this->status_set_error(LOG_STR("Firmware file not readable"));
    return false;
  }
  const storage::StorageError err =
      storage::global_storage_worker->begin_read(ps, rel, &this->stream_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGE(TAG, "Opening the firmware failed (%s)", storage::error_to_string(e));
          this->status_set_error(LOG_STR("Firmware read failed"));
          this->install_done_(false);
          return;
        }
        this->start_ota_();
      });
  this->stream_open_ = err == storage::StorageError::STORAGE_ERROR_OK;
  if (!this->stream_open_)
    this->status_set_error(LOG_STR("Firmware read failed"));
  return this->stream_open_;
}

void Esp32HostedUpdate::read_storage_() {
  const size_t want = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - this->offset_));
  this->got_ = 0;
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->stream_, this->chunk_, want, &this->got_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->got_ == 0) {
          ESP_LOGE(TAG, "Read failed at %llu", static_cast<unsigned long long>(this->offset_));
          this->status_set_error(LOG_STR("Firmware read failed"));
          this->install_done_(false);
          return;
        }
        this->on_chunk_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->status_set_error(LOG_STR("Firmware read failed"));
    this->install_done_(false);
  }
}
#endif

void Esp32HostedUpdate::install_failed_(const LogString *what) {
  this->status_set_error(what);
  this->busy_ = false;
  this->state_ = this->prev_state_;
  this->publish_state();
  this->progress_trigger_.trigger(-1);
}

void Esp32HostedUpdate::enter_safe_() {
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
  if (this->safe_mode_ != nullptr) {
    if (this->safe_mode_->get_level() == safe_mode::LEVEL_NORMAL) {
      if (!this->safe_mode_->switch_to_safe_mode()) {
        ESP_LOGW(TAG, "C6 update refused: %s", this->safe_mode_->get_refusal());
        this->status_set_error(LOG_STR("Safe mode refused"));
        this->busy_ = false;
        this->state_ = this->prev_state_;
        this->publish_state();
        return;
      }
      this->normal_after_flash_ = true;
    }
    if (!this->safe_mode_->is_safe_ready()) {
      this->await_safe_ = true;
      return;
    }
  }
#endif
  this->safe_install_();
}

void Esp32HostedUpdate::safe_install_() {
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  if (this->method_ == UpdateMethod::STORAGE && this->is_manifest_()) {
    this->state_ = this->prev_state_;
    this->install_after_check_ = true;
    this->storage_check_();
    return;
  }
#endif
  this->begin_install_();
}

#if defined(USE_ESP32_HOSTED_SERIAL) && \
    (defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE))
void Esp32HostedUpdate::perform_serial() {
  if (this->busy_) {
    ESP_LOGW(TAG, "Busy");
    return;
  }
  this->serial_wanted_ = true;
  this->perform(true);
  if (!this->busy_)
    this->serial_wanted_ = false;
}
#endif

void Esp32HostedUpdate::perform(bool force) {
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
  if (this->await_safe_ && this->safe_mode_ != nullptr && this->safe_mode_->get_level() == safe_mode::LEVEL_NORMAL) {
    this->await_safe_ = false;
    this->busy_ = false;
    this->state_ = this->prev_state_;
  }
#endif
  if (this->busy_) {
    ESP_LOGW(TAG, "Busy");
    return;
  }
  bool recheck = false;
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  recheck = this->method_ == UpdateMethod::STORAGE && this->is_manifest_();
#endif
  if (!recheck && this->state_ != update::UPDATE_STATE_AVAILABLE && !force) {
    ESP_LOGW(TAG, "Update not available");
    return;
  }

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  if (this->method_ == UpdateMethod::HTTP && this->firmware_url_.empty()) {
    ESP_LOGW(TAG, "No firmware URL available, run check first");
    return;
  }
#endif

#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  if (this->method_ == UpdateMethod::EMBEDDED) {
    if (this->firmware_data_ == nullptr || this->firmware_size_ == 0) {
      ESP_LOGE(TAG, "No firmware data available");
      this->status_set_error(LOG_STR("No firmware data available"));
      return;
    }
    // Verify SHA256 before writing
    sha256::SHA256 hasher;
    hasher.init();
    hasher.add(this->firmware_data_, this->firmware_size_);
    hasher.calculate();
    if (!hasher.equals_bytes(this->firmware_sha256_.data())) {
      ESP_LOGE(TAG, "SHA256 mismatch");
      this->status_set_error(LOG_STR("SHA256 verification failed"));
      return;
    }
  }
#endif

  this->prev_state_ = this->state_;
  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->update_info_.has_progress = false;
  this->publish_state();
  this->busy_ = true;
  this->force_ = force;
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  this->fallback_ = false;
#endif
  this->enter_safe_();
}

void Esp32HostedUpdate::begin_install_() {
#if defined(USE_ESP32_HOSTED_SERIAL) && \
    (defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE))
  if (this->serial_wanted_) {
    this->serial_wanted_ = false;
    if (!this->serial_enabled_ || !this->factory_known_) {
      ESP_LOGE(TAG, "No C6 factory image or no serial route");
      this->install_failed_(LOG_STR("No C6 factory image"));
      return;
    }
    this->fallback_ = true;
    this->firmware_url_ = this->factory_url_;
    this->firmware_sha256_ = this->factory_sha256_;
  }
#endif
  this->chunk_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(CHUNK_SIZE);
  if (this->chunk_ == nullptr) {
    ESP_LOGE(TAG, "No memory for the transfer");
    this->status_set_error(LOG_STR("No memory"));
    this->install_done_(false);
    return;
  }
  this->offset_ = 0;
  this->hasher_.init();
  switch (this->method_) {
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
    case UpdateMethod::HTTP:
      if (!this->open_download_()) {
        this->install_done_(false);
        return;
      }
      this->start_ota_();
      return;
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
    case UpdateMethod::STORAGE:
      if (!this->open_storage_read_())
        this->install_done_(false);
      return;
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
    case UpdateMethod::EMBEDDED:
      ESP_LOGI(TAG, "Starting OTA update (%zu bytes)", this->firmware_size_);
      this->total_ = this->firmware_size_;
      this->start_ota_();
      return;
#endif
    default:
      this->install_done_(false);
      return;
  }
}

static bool other_network() {
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected())
    return true;
#endif
#ifdef USE_USB_ETHERNET
  if (usb_ethernet::global_usb_ethernet != nullptr && usb_ethernet::global_usb_ethernet->is_connected())
    return true;
#endif
  return false;
}

void Esp32HostedUpdate::update_announce_() {
  this->busy_ = true;
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
  if (this->safe_mode_ != nullptr && this->safe_mode_->get_level() == safe_mode::LEVEL_NORMAL) {
    this->normal_after_flash_ = true;
    this->safe_mode_->switch_to_safe_mode();
  }
#endif
  this->progress_trigger_.trigger(0);
  if (other_network()) {
    this->update_start_();
    return;
  }
  ESP_LOGW(TAG, "Wi-Fi is the only network: offline during the C6 update, back by itself");
  this->update_info_.summary = "C6 update: Wi-Fi goes down, the device comes back by itself";
  this->publish_state();
  this->set_timeout(UPDATE_NOTICE_MS, [this]() { this->update_start_(); });
}

void Esp32HostedUpdate::update_start_() {
  this->update_pause_();
  TaskFunction_t task = slot_task_;
#ifdef USE_ESP32_HOSTED_SERIAL
  if (this->serial_route_ && !this->slot_return_)
    task = serial_flash_task_;
#endif
  if (this->flash_task_.create(task, "c6_update", UPDATE_TASK_STACK, this, UPDATE_TASK_PRIORITY, true))
    return;
  ESP_LOGE(TAG, "No task for the C6 update");
  this->update_resume_(false, true);
}

void Esp32HostedUpdate::update_pause_() {
#ifdef USE_WIFI
  if (!this->wifi_after_flash_ && wifi::global_wifi_component != nullptr &&
      !wifi::global_wifi_component->is_disabled()) {
    this->wifi_after_flash_ = true;
    wifi::global_wifi_component->disable();
    esp_wifi_deinit();
  }
#endif
#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD
  usb_bluetooth::c6_offload::set_paused(true);
#endif
}

void Esp32HostedUpdate::update_progress_(void *ctx, uint8_t percent) {
  auto *self = static_cast<Esp32HostedUpdate *>(ctx);
  ESP_LOGI(TAG, "C6 update %u %%", percent);
  self->defer([self, percent]() {
    self->update_info_.has_progress = true;
    self->update_info_.progress = percent;
    self->publish_state();
    self->progress_trigger_.trigger(percent);
  });
}

void Esp32HostedUpdate::slot_on_state_(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx) {
  auto *self = static_cast<Esp32HostedUpdate *>(ctx);
  if (len < SLOT_STATE_MIN)
    return;
  self->slot_ = {};
  std::memcpy(&self->slot_, data, std::min<size_t>(len, sizeof(SlotState)));
  TaskHandle_t waiter = self->slot_waiter_;
  if (waiter != nullptr) {
    xTaskNotifyGive(waiter);
    return;
  }
  self->defer([self]() { self->slot_answered_(); });
}

void Esp32HostedUpdate::slot_query_(bool install) {
  this->slot_install_ = install;
  this->slot_ = {};
  esp_hosted_register_custom_callback(SLOT_MSG_STATE, slot_on_state_, this);
  const uint8_t command = SLOT_QUERY;
  this->set_timeout(SLOT_TIMEOUT_ID, SLOT_ANSWER_MS, [this]() { this->slot_no_answer_(); });
  if (esp_hosted_send_custom_data(SLOT_MSG_COMMAND, &command, sizeof(command)) != ESP_OK) {
    this->cancel_timeout(SLOT_TIMEOUT_ID);
    this->slot_no_answer_();
  }
}

bool Esp32HostedUpdate::offload_offered_() const {
  return this->update_info_.latest_version.find("+xbt.") != std::string::npos;
}

bool Esp32HostedUpdate::stock_offered_() const {
  if (this->method_ == UpdateMethod::EMBEDDED)
    return false;
  const std::string &version = this->update_info_.latest_version;
  return !version.empty() && version != "storage" && version.find("+xbt.") == std::string::npos;
}

void Esp32HostedUpdate::switch_reset_() {
  if (this->main_image_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->main_image_, this->main_total_);
    this->main_image_ = nullptr;
  }
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  if (this->minimal_first_) {
    this->firmware_url_ = this->main_url_;
    this->firmware_sha256_ = this->main_sha256_;
  }
#endif
  this->minimal_first_ = false;
}

void Esp32HostedUpdate::slot_no_answer_() {
  if (!this->slot_install_)
    return;
  this->slot_install_ = false;
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  if (this->minimal_first_) {
    this->update_announce_();
    return;
  }
  if (this->offload_offered_()) {
    if (!this->minimal_known_) {
      ESP_LOGE(TAG, "No minimal image in the manifest: the stock C6 firmware stays");
      this->status_set_error(LOG_STR("No minimal image"));
      this->image_free_();
      this->busy_ = false;
      this->state_ = this->prev_state_;
      this->publish_state();
      return;
    }
    ESP_LOGI(TAG, "C6 runs the stock firmware: minimal image first");
    this->fetch_minimal_();
    return;
  }
#endif
  this->ota_image_start_();
}

void Esp32HostedUpdate::fetch_minimal_() {
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  this->main_image_ = this->image_;
  this->main_total_ = this->total_;
  this->image_ = nullptr;
  this->main_url_ = this->firmware_url_;
  this->main_sha256_ = this->firmware_sha256_;
  this->firmware_url_ = this->minimal_url_;
  this->firmware_sha256_ = this->minimal_sha256_;
  this->minimal_first_ = true;
  this->begin_install_();
#endif
}

void Esp32HostedUpdate::slot_answered_() {
  this->cancel_timeout(SLOT_TIMEOUT_ID);
  ESP_LOGD(TAG, "C6 slot: %s, other slot %s", this->slot_.minimal != 0 ? "minimal" : "main",
           this->slot_.other_valid != 0 ? "valid" : "empty");
  if (this->slot_install_) {
    this->slot_install_ = false;
    if (this->slot_.minimal == 0 && this->slot_.other_valid == 0) {
      ESP_LOGE(TAG, "The C6 has no minimal firmware: no update over esp_hosted");
      this->status_set_error(LOG_STR("C6 has no minimal firmware"));
      this->image_free_();
      this->busy_ = false;
      this->state_ = this->prev_state_;
      this->publish_state();
      return;
    }
#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
    if (!this->minimal_first_ && this->offload_offered_() && this->minimal_known_ &&
        this->slot_.revision < this->minimal_revision_) {
      ESP_LOGI(TAG, "C6 minimal image revision %u, offered %u: it is renewed first", this->slot_.revision,
               this->minimal_revision_);
      this->fetch_minimal_();
      return;
    }
#endif
    this->update_announce_();
    return;
  }
  if (this->slot_.minimal != 0 && this->slot_.other_valid != 0 && !this->busy_) {
    ESP_LOGW(TAG, "C6 is in its minimal firmware: back to main");
    this->slot_return_ = true;
    this->update_announce_();
  } else if (this->slot_.minimal != 0 && this->slot_.other_valid == 0) {
    ESP_LOGW(TAG, "C6 is in its minimal firmware and has no main firmware: update it");
    this->update_info_.summary = "C6 has no main firmware";
    this->status_set_error(LOG_STR("C6 has no main firmware"));
    this->publish_state();
  }
}

bool Esp32HostedUpdate::slot_command_(uint8_t command, uint8_t arg, uint32_t wait_ms) {
  const uint8_t message[2] = {command, arg};
  const size_t len = command == SLOT_TABLE ? 2 : 1;
  this->slot_waiter_ = xTaskGetCurrentTaskHandle();
  ulTaskNotifyTake(pdTRUE, 0);
  bool answered = esp_hosted_send_custom_data(SLOT_MSG_COMMAND, message, len) == ESP_OK;
  answered = answered && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms != 0 ? wait_ms : SLOT_ANSWER_MS)) > 0;
  this->slot_waiter_ = nullptr;
  if (answered && this->slot_.result != 0)
    ESP_LOGE(TAG, "C6 slot command %u failed: 0x%x", command,
             static_cast<unsigned>(this->slot_.error_low | (this->slot_.error_high << 8)));
  return answered && this->slot_.result == 0;
}

bool Esp32HostedUpdate::hosted_restart_() {
  esp_hosted_deinit();
  int rc = esp_hosted_init();
  if (rc == 0)
    rc = esp_hosted_connect_to_slave();
  this->hosted_up_ = rc == 0;
  if (this->hosted_up_) {
    ESP_LOGI(TAG, "The C6 is ready");
  } else {
    ESP_LOGE(TAG, "The C6 did not come back: %d", rc);
  }
  return this->hosted_up_;
}

bool Esp32HostedUpdate::slot_switch_(bool minimal) {
  uint8_t command = SLOT_BOOT_OTHER;
  if (this->slot_.commands >= SLOT_BOOT_MINIMAL)
    command = minimal ? SLOT_BOOT_MINIMAL : SLOT_BOOT_MAIN;
  if (!this->slot_command_(command) || !this->hosted_restart_() || !this->slot_command_(SLOT_QUERY))
    return false;
  return (this->slot_.minimal != 0) == minimal;
}

bool Esp32HostedUpdate::slot_write_() {
  esp_err_t err = esp_hosted_slave_ota_begin();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    return false;
  }
  uint8_t reported = 0;
  for (uint64_t at = 0; at < this->total_;) {
    const size_t len = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - at));
    err = esp_hosted_slave_ota_write(this->image_ + at, len);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write OTA data at %llu: %s", static_cast<unsigned long long>(at), esp_err_to_name(err));
      esp_hosted_slave_ota_end();
      return false;
    }
    at += len;
    const uint8_t percent = static_cast<uint8_t>(at * 100 / this->total_);
    if (percent >= reported + SLOT_PROGRESS_STEP) {
      reported = percent;
      update_progress_(this, percent);
    }
  }
  err = esp_hosted_slave_ota_end();
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
  return err == ESP_OK;
}

bool Esp32HostedUpdate::install_with_minimal_() {
  if (this->slot_.commands == 0) {
    if (!this->slot_write_())
      return false;
    const esp_err_t err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to activate the minimal image: %s", esp_err_to_name(err));
      return false;
    }
    if (!this->hosted_restart_() || !this->slot_command_(SLOT_QUERY))
      return false;
  } else {
    if (this->slot_.minimal == 0 && !this->slot_switch_(true))
      return false;
    if (!this->slot_write_())
      return false;
    if (!this->slot_command_(this->slot_.commands >= SLOT_BOOT_MINIMAL ? SLOT_BOOT_MAIN : SLOT_BOOT_OTHER) ||
        !this->hosted_restart_() || !this->slot_command_(SLOT_QUERY))
      return false;
  }
  if (this->slot_.minimal == 0 || this->slot_.commands < SLOT_TABLE) {
    ESP_LOGE(TAG, "The C6 does not run a minimal image that can switch");
    return false;
  }
  if (this->slot_.table != SLOT_TABLE_OURS) {
    if (this->slot_.first == 0 && !this->slot_command_(SLOT_CLONE, 0, SLOT_CLONE_MS))
      return false;
    if (!this->slot_command_(SLOT_TABLE, SLOT_TABLE_OURS, SLOT_TABLE_MS) || !this->hosted_restart_() ||
        !this->slot_command_(SLOT_QUERY))
      return false;
    if (this->slot_.table != SLOT_TABLE_OURS || this->slot_.first == 0) {
      ESP_LOGE(TAG, "The C6 did not come up on the new partition table");
      return false;
    }
  }
  if (this->slot_.first != 0) {
    if (!this->slot_command_(SLOT_CLONE, 0, SLOT_CLONE_MS) || !this->slot_command_(SLOT_BOOT_OTHER) ||
        !this->hosted_restart_() || !this->slot_command_(SLOT_QUERY))
      return false;
    if (this->slot_.first != 0 || this->slot_.minimal == 0) {
      ESP_LOGE(TAG, "The C6 did not start its minimal image from its own slot");
      return false;
    }
  }
  this->image_free_();
  this->image_ = this->main_image_;
  this->total_ = this->main_total_;
  this->main_image_ = nullptr;
  return this->slot_write_();
}

void Esp32HostedUpdate::slot_task_(void *arg) {
  auto *self = static_cast<Esp32HostedUpdate *>(arg);
  bool ok = true;
  self->hosted_up_ = true;
  if (self->minimal_first_) {
    ok = self->install_with_minimal_();
  } else if (!self->slot_return_) {
    if (self->slot_.minimal == 0)
      ok = self->slot_switch_(true);
    const bool stock = ok && self->stock_offered_();
    if (stock && (self->slot_.commands < SLOT_TABLE || self->total_ > STOCK_APP_SIZE)) {
      ESP_LOGE(TAG, "Stock firmware not written: %s",
               self->total_ > STOCK_APP_SIZE ? "image too large for the stock slot" : "the C6 minimal image is too old");
      ok = false;
    }
    if (ok)
      ok = self->slot_write_();
    if (ok && stock) {
      ok = self->slot_command_(SLOT_TABLE, SLOT_TABLE_STOCK, SLOT_TABLE_MS) && self->hosted_restart_();
      if (ok)
        self->slot_ = {};
    }
  }
  if (self->hosted_up_ && self->slot_.minimal != 0) {
#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD
    usb_bluetooth::c6_offload::set_paused(false);
#endif
    const bool main_up = self->slot_switch_(false);
    ok = ok && main_up;
  }
  const bool back = self->hosted_up_;
  self->defer([self, ok, back]() { self->update_resume_(ok, back); });
  vTaskSuspend(nullptr);
}

#ifdef USE_ESP32_HOSTED_SERIAL
void Esp32HostedUpdate::serial_flash_task_(void *arg) {
  auto *self = static_cast<Esp32HostedUpdate *>(arg);
  self->log_after_flash_ = self->log_on_ || self->log_restart_;
  const bool reader = self->log_task_.is_created();
  self->serial_log_stop_();
  while (reader && !self->log_parked_.load(std::memory_order_acquire))
    vTaskDelay(1);
  esp_hosted_deinit();
  bool ok = self->serial_.open();
  if (ok) {
    self->serial_.enter_bootloader();
    ok = self->serial_.flash(self->image_, static_cast<uint32_t>(self->total_), update_progress_, self);
    self->serial_.close();
  }
  self->serial_.leave_bootloader();
  int rc = esp_hosted_init();
  if (rc == 0)
    rc = esp_hosted_connect_to_slave();
  const bool back = rc == 0;
  if (back) {
    ESP_LOGI(TAG, "The C6 is ready");
  } else {
    ESP_LOGE(TAG, "The C6 did not come back: %d", rc);
  }
  self->defer([self, ok, back]() { self->update_resume_(ok, back); });
  vTaskSuspend(nullptr);
}
#endif

void Esp32HostedUpdate::update_task_free_() {
  if (!this->flash_task_.is_created())
    return;
  if (!this->flash_task_.deallocate())
    this->set_timeout(UPDATE_FREE_RETRY_MS, [this]() { this->update_task_free_(); });
}

void Esp32HostedUpdate::update_resume_(bool ok, bool back) {
  this->update_task_free_();
  this->switch_reset_();
#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD
  usb_bluetooth::c6_offload::set_paused(false);
#endif
#ifdef USE_WIFI
  if (this->wifi_after_flash_ && back) {
    this->wifi_after_flash_ = false;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != nullptr)
      esp_netif_action_stop(sta, nullptr, 0, nullptr);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK)
      err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "Wi-Fi not started again: %s", esp_err_to_name(err));
    wifi::global_wifi_component->enable();
  }
#endif
  this->image_free_();
  this->busy_ = false;
  this->update_info_.has_progress = false;
#ifdef USE_ESP32_HOSTED_SERIAL
  if (this->log_after_flash_) {
    this->log_after_flash_ = false;
    this->serial_log_start_();
  }
#endif
  ok = ok && back;
  const bool returned = this->slot_return_;
  this->slot_return_ = false;
  if (back)
    this->read_version_();
  if (ok && !returned && !this->update_info_.latest_version.empty() && this->update_info_.latest_version != "storage" &&
      this->update_info_.latest_version != this->update_info_.current_version) {
    ESP_LOGE(TAG, "C6 runs %s after the update to %s", this->update_info_.current_version.c_str(),
             this->update_info_.latest_version.c_str());
    ok = false;
  }
#if defined(USE_ESP32_HOSTED_SERIAL) && \
    (defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE))
  if (!ok && !returned && !this->serial_route_ && this->serial_enabled_ && this->factory_known_ && !this->fallback_) {
    ESP_LOGW(TAG, "C6 update over esp_hosted failed: factory image over serial");
    this->fallback_ = true;
    this->firmware_url_ = this->factory_url_;
    this->firmware_sha256_ = this->factory_sha256_;
    this->busy_ = true;
    this->begin_install_();
    return;
  }
  this->fallback_ = false;
#endif
  if (ok) {
    ESP_LOGI(TAG, "C6 %s, version %s", returned ? "back on main" : "updated", this->update_info_.current_version.c_str());
    this->update_info_.summary = "";
    if (!returned)
      this->state_ = update::UPDATE_STATE_NO_UPDATE;
    this->status_clear_error();
  } else {
    ESP_LOGE(TAG, "C6 update failed");
    this->update_info_.summary = "C6 update failed";
    if (!returned)
      this->state_ = this->prev_state_;
    this->status_set_error(LOG_STR("C6 update failed"));
  }
#if defined(USE_ESP32_HOSTED_UPDATE_SAFE_MODE) && defined(USE_SAFE_MODE_SPLIT)
  if (ok && this->normal_after_flash_) {
    this->normal_after_flash_ = false;
    this->safe_mode_->go_normal();
  }
#endif
  this->publish_state();
  this->progress_trigger_.trigger(ok ? 100 : -1);
}

#ifdef USE_ESP32_HOSTED_SERIAL
void Esp32HostedUpdate::serial_settings_load_() {
  this->serial_pref_ = this->make_entity_preference<SerialSettings>();
  SerialSettings saved{};
  if (!this->serial_pref_.load(&saved))
    return;
  this->serial_enabled_ = saved.enabled != 0;
  this->serial_.set_pins(saved.uart_num, saved.tx_pin, saved.rx_pin, saved.boot_pin);
}

void Esp32HostedUpdate::set_serial_settings(bool enabled, uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin) {
  if (this->busy_)
    return;
  this->serial_log_stop_();
  this->serial_enabled_ = enabled;
  this->serial_.set_pins(uart_num, tx_pin, rx_pin, boot_pin);
  SerialSettings saved{static_cast<uint8_t>(enabled), uart_num, static_cast<int8_t>(tx_pin), static_cast<int8_t>(rx_pin),
                       static_cast<int8_t>(boot_pin)};
  this->serial_pref_.save(&saved);
  ESP_LOGI(TAG, "Serial route %s: UART %u, tx %d, rx %d, boot %d", enabled ? "on" : "off", uart_num, tx_pin, rx_pin,
           boot_pin);
}

void Esp32HostedUpdate::serial_log_task_(void *arg) {
  auto *self = static_cast<Esp32HostedUpdate *>(arg);
  self->serial_.read_log(self->log_stop_);
  self->serial_.close();
  self->log_parked_.store(true, std::memory_order_release);
  self->defer([self]() { self->serial_log_parked_(); });
  vTaskSuspend(nullptr);
}

void Esp32HostedUpdate::serial_log_parked_() {
  if (!this->log_task_.deallocate()) {
    this->set_timeout(UPDATE_FREE_RETRY_MS, [this]() { this->serial_log_parked_(); });
    return;
  }
  if (!this->log_restart_)
    return;
  this->log_restart_ = false;
  if (this->busy_) {
    this->log_after_flash_ = true;
    return;
  }
  this->serial_log_start_();
}

bool Esp32HostedUpdate::serial_log_start_() {
  if (this->log_on_)
    return true;
  if (!this->serial_enabled_)
    return false;
  if (this->log_task_.is_created()) {
    this->log_restart_ = true;
    return true;
  }
  if (!this->serial_.open())
    return false;
  this->log_stop_.store(false, std::memory_order_release);
  this->log_parked_.store(false, std::memory_order_release);
  if (!this->log_task_.create(serial_log_task_, "c6_uart_log", SERIAL_LOG_STACK, this, SERIAL_LOG_PRIORITY, true)) {
    ESP_LOGE(TAG, "No task for the C6 UART log");
    this->serial_.close();
    return false;
  }
  this->log_on_ = true;
  return true;
}

void Esp32HostedUpdate::serial_log_stop_() {
  this->log_restart_ = false;
  if (!this->log_on_)
    return;
  this->log_on_ = false;
  this->log_stop_.store(true, std::memory_order_release);
}

void Esp32HostedUpdate::set_serial_log(bool on) {
  if (this->busy_) {
    this->log_after_flash_ = on;
    return;
  }
  if (on) {
    this->serial_log_start_();
  } else {
    this->serial_log_stop_();
  }
}
#endif

void Esp32HostedUpdate::start_ota_() {
  this->image_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(this->total_);
  if (this->image_ == nullptr) {
    ESP_LOGE(TAG, "No memory for the image");
    this->status_set_error(LOG_STR("No memory"));
    this->install_done_(false);
    return;
  }
  this->last_progress_ = millis();
  this->next_chunk_();
}

void Esp32HostedUpdate::image_free_() {
  if (this->image_ == nullptr)
    return;
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->image_, this->total_);
  this->image_ = nullptr;
}

void Esp32HostedUpdate::publish_progress_() {
  const uint32_t now = millis();
  if (now - this->last_progress_ < PROGRESS_MS)
    return;
  this->last_progress_ = now;
  this->update_info_.has_progress = true;
  this->update_info_.progress = this->offset_ * 100.0f / this->total_;
  this->publish_state();
}

bool Esp32HostedUpdate::app_image_ok_() const {
  constexpr size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  return this->total_ >= desc_offset + sizeof(esp_app_desc_t) && this->image_[0] == ESP_IMAGE_HEADER_MAGIC &&
         reinterpret_cast<const esp_app_desc_t *>(this->image_ + desc_offset)->magic_word == ESP_APP_DESC_MAGIC_WORD;
}

void Esp32HostedUpdate::ota_image_start_() {
  esp_err_t err;
  {
    watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
    err = esp_hosted_slave_ota_begin();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    this->ota_image_failed_(LOG_STR("Failed to begin OTA"));
    return;
  }
  this->offset_ = 0;
  this->last_progress_ = millis();
  this->ota_image_step_();
}

void Esp32HostedUpdate::ota_image_step_() {
  if (this->offset_ >= this->total_) {
    this->image_free_();
    this->ota_finish_();
    return;
  }
  const size_t len = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - this->offset_));
  const esp_err_t err = esp_hosted_slave_ota_write(this->image_ + this->offset_, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write OTA data at %llu of %llu bytes: %s", static_cast<unsigned long long>(this->offset_),
             static_cast<unsigned long long>(this->total_), esp_err_to_name(err));
    ota_end_guarded();
    this->ota_image_failed_(LOG_STR("Failed to write OTA data"));
    return;
  }
  this->offset_ += len;
  this->publish_progress_();
  this->defer([this]() { this->ota_image_step_(); });
}

void Esp32HostedUpdate::ota_image_failed_(const LogString *what) {
  this->status_set_error(what);
  this->image_free_();
  this->update_info_.has_progress = false;
  this->busy_ = false;
  this->state_ = this->prev_state_;
  this->publish_state();
}

void Esp32HostedUpdate::next_chunk_() {
  if (this->offset_ >= this->total_) {
    this->install_done_(true);
    return;
  }
  switch (this->method_) {
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
    case UpdateMethod::HTTP:
      this->defer([this]() { this->read_download_(); });
      return;
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
    case UpdateMethod::STORAGE:
      this->read_storage_();
      return;
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
    case UpdateMethod::EMBEDDED:
      this->defer([this]() {
        this->got_ = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - this->offset_));
        memcpy(this->chunk_, this->firmware_data_ + this->offset_, this->got_);
        this->on_chunk_();
      });
      return;
#endif
    default:
      this->install_done_(false);
      return;
  }
}

void Esp32HostedUpdate::on_chunk_() {
  this->hasher_.add(this->chunk_, this->got_);
  std::memcpy(this->image_ + this->offset_, this->chunk_, this->got_);
  this->offset_ += this->got_;
  this->publish_progress_();
  this->next_chunk_();
}

void Esp32HostedUpdate::install_done_(bool ok) {
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  if (this->container_ != nullptr) {
    this->container_->end();
    this->container_.reset();
  }
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  this->close_stream_();
#endif
  if (ok) {
    // Verify SHA256 over the streamed bytes
    this->hasher_.calculate();
    ok = this->hasher_.equals_bytes(this->firmware_sha256_.data());
    if (!ok) {
      ESP_LOGE(TAG, "SHA256 mismatch");
      this->status_set_error(LOG_STR("SHA256 verification failed"));
    } else {
      ESP_LOGI(TAG, "SHA256 verified successfully");
    }
  }
  this->finish_install_(ok);
}

void Esp32HostedUpdate::finish_install_(bool ok) {
  if (this->chunk_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->chunk_, CHUNK_SIZE);
    this->chunk_ = nullptr;
  }
  this->update_info_.has_progress = false;
  if (ok) {
    if (this->app_image_ok_()) {
#ifdef USE_ESP32_HOSTED_SERIAL
      this->serial_route_ = false;
#endif
      this->slot_query_(true);
      return;
    }
#ifdef USE_ESP32_HOSTED_SERIAL
    if (this->serial_enabled_ && C6Serial::image_ok(this->image_, static_cast<uint32_t>(this->total_))) {
      this->serial_route_ = true;
      this->update_announce_();
      return;
    }
    if (this->serial_enabled_)
      this->status_set_error(LOG_STR("No C6 factory image"));
    else
#endif
    {
      ESP_LOGE(TAG, "No app image: the coprocessor is not touched");
      this->status_set_error(LOG_STR("No app image"));
    }
  }
  this->switch_reset_();
  this->image_free_();
  this->busy_ = false;
  this->state_ = this->prev_state_;
  this->publish_state();
  this->progress_trigger_.trigger(-1);
}

void Esp32HostedUpdate::ota_finish_() {
  this->update_info_.has_progress = false;
  this->busy_ = false;
  esp_err_t end_err = ota_end_guarded();
  if (end_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(end_err));
    this->state_ = this->prev_state_;
    this->status_set_error(LOG_STR("Failed to end OTA"));
    this->publish_state();
    return;
  }

  // Update state
  ESP_LOGI(TAG, "OTA update successful");
  this->state_ = update::UPDATE_STATE_NO_UPDATE;
  this->status_clear_error();
  this->publish_state();

#ifdef USE_OTA_ROLLBACK
  // Mark the host partition as valid before rebooting, in case the safe mode
  // timer hasn't expired yet.
  if (esp32::system_info::has_otadata())
    esp_ota_mark_app_valid_cancel_rollback();
#endif

  // Give the state publish time to reach the API clients, then restart both sides
  ESP_LOGI(TAG, "Restarting in 1 second");
  this->set_timeout(1000, []() {
    // Shut the host down while the coprocessor still answers; activate makes it
    // reboot itself 500 ms later
    App.safe_shutdown(TEARDOWN_TIMEOUT_REBOOT_MS);
    esp_err_t err = esp_hosted_slave_ota_activate();  // NOLINT
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to activate OTA: %s", esp_err_to_name(err));
    }
    // esp_wifi_remote registers esp_wifi_stop with esp_restart; that RPC to the
    // rebooting coprocessor blocks until the task watchdog fires
    esp_unregister_shutdown_handler(reinterpret_cast<shutdown_handler_t>(esp_wifi_stop));
    arch_restart();
  });
}

}  // namespace esphome::esp32_hosted
#endif
