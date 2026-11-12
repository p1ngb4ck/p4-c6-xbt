#include "c6_serial.h"

#ifdef USE_ESP32_HOSTED_SERIAL

#include "esphome/components/md5/md5.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

extern "C" int eh_host_port_reset_slave(void);

namespace esphome::esp32_hosted {

ESPHOME_LOG_TAG(TAG, "esp32_hosted.serial");

static constexpr uint8_t SLIP_END = 0xC0;
static constexpr uint8_t SLIP_ESC = 0xDB;
static constexpr uint8_t SLIP_ESC_END = 0xDC;
static constexpr uint8_t SLIP_ESC_ESC = 0xDD;

static constexpr uint8_t OP_FLASH_BEGIN = 0x02;
static constexpr uint8_t OP_FLASH_DATA = 0x03;
static constexpr uint8_t OP_SYNC = 0x08;
static constexpr uint8_t OP_SPI_SET_PARAMS = 0x0B;
static constexpr uint8_t OP_SPI_ATTACH = 0x0D;
static constexpr uint8_t OP_CHANGE_BAUDRATE = 0x0F;
static constexpr uint8_t OP_SPI_FLASH_MD5 = 0x13;

static constexpr uint8_t IMAGE_MAGIC = 0xE9;
static constexpr uint32_t IMAGE_CHIP_OFFSET = 12;
static constexpr uint8_t IMAGE_CHIP_C6 = 0x0D;
static constexpr uint32_t PARTITION_TABLE_OFFSET = 0x8000;
static constexpr uint8_t PARTITION_MAGIC_0 = 0xAA;
static constexpr uint8_t PARTITION_MAGIC_1 = 0x50;
static constexpr uint32_t ROM_BAUD = 115200;
static constexpr size_t UART_RX_BUFFER = 2048;
static constexpr size_t SLIP_CHUNK = 128;
static constexpr size_t FRAME_MAX = 64;
static constexpr uint32_t FLASH_BLOCK = 0x400;
static constexpr uint32_t CHECKSUM_START = 0xEF;
static constexpr uint32_t FLASH_LIMIT = 16u * 1024u * 1024u;
static constexpr uint32_t FLASH_ERASE_BLOCK = 64u * 1024u;
static constexpr uint32_t FLASH_SECTOR = 4096;
static constexpr uint32_t FLASH_PAGE = 256;
static constexpr uint32_t FLASH_STATUS_MASK = 0xFFFF;
static constexpr int SYNC_ATTEMPTS = 40;
static constexpr int WRITE_ATTEMPTS = 3;
static constexpr uint32_t SYNC_TIMEOUT_MS = 100;
static constexpr uint32_t COMMAND_TIMEOUT_MS = 3000;
static constexpr uint32_t ERASE_MS_PER_MB = 40000;
static constexpr uint32_t MD5_MS_PER_MB = 8000;
static constexpr uint32_t BAUD_SETTLE_MS = 50;
static constexpr uint32_t SYNC_SETTLE_MS = 100;
static constexpr uint32_t BOOT_SETTLE_MS = 100;
static constexpr size_t MD5_HEX_LEN = 32;
static constexpr size_t LOG_LINE = 128;
static constexpr uint32_t LOG_READ_MS = 200;

static uint32_t scaled_timeout(uint32_t ms_per_mb, uint32_t size) {
  const uint64_t timeout = static_cast<uint64_t>(ms_per_mb) * size / 1000000u;
  return std::max<uint32_t>(static_cast<uint32_t>(timeout), COMMAND_TIMEOUT_MS);
}

static void put_u32(uint8_t *out, uint32_t value) { std::memcpy(out, &value, sizeof(value)); }

void C6Serial::set_config(uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin, uint32_t baud_rate) {
  this->uart_num_ = uart_num;
  this->tx_pin_ = tx_pin;
  this->rx_pin_ = rx_pin;
  this->boot_pin_ = boot_pin;
  this->baud_rate_ = baud_rate;
}

void C6Serial::set_pins(uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin) {
  this->uart_num_ = uart_num;
  this->tx_pin_ = tx_pin;
  this->rx_pin_ = rx_pin;
  this->boot_pin_ = boot_pin;
}

bool C6Serial::image_ok(const uint8_t *image, uint32_t size) {
  if (size <= PARTITION_TABLE_OFFSET + 1 || size > FLASH_LIMIT || image[0] != IMAGE_MAGIC ||
      image[IMAGE_CHIP_OFFSET] != IMAGE_CHIP_C6 || image[IMAGE_CHIP_OFFSET + 1] != 0 ||
      image[PARTITION_TABLE_OFFSET] != PARTITION_MAGIC_0 || image[PARTITION_TABLE_OFFSET + 1] != PARTITION_MAGIC_1) {
    ESP_LOGE(TAG, "No C6 factory image (%" PRIu32 " bytes): nothing stopped, nothing written", size);
    return false;
  }
  return true;
}

bool C6Serial::open() {
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  uart_config_t config{};
  config.baud_rate = ROM_BAUD;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  esp_err_t err = uart_driver_install(port, UART_RX_BUFFER, 0, 0, nullptr, 0);
  if (err == ESP_OK)
    err = uart_param_config(port, &config);
  if (err == ESP_OK)
    err = uart_set_pin(port, this->tx_pin_, this->rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UART %u not opened: %s", this->uart_num_, esp_err_to_name(err));
    uart_driver_delete(port);
    return false;
  }
  this->open_uart_ = this->uart_num_;
  this->open_tx_ = this->tx_pin_;
  this->open_rx_ = this->rx_pin_;
  return true;
}

void C6Serial::close() {
  uart_driver_delete(static_cast<uart_port_t>(this->open_uart_));
  gpio_reset_pin(static_cast<gpio_num_t>(this->open_tx_));
  gpio_reset_pin(static_cast<gpio_num_t>(this->open_rx_));
}

void C6Serial::enter_bootloader() {
  const auto pin = static_cast<gpio_num_t>(this->boot_pin_);
  gpio_set_level(pin, 0);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(pin, 0);
  eh_host_port_reset_slave();
  vTaskDelay(pdMS_TO_TICKS(BOOT_SETTLE_MS));
}

void C6Serial::leave_bootloader() {
  gpio_reset_pin(static_cast<gpio_num_t>(this->boot_pin_));
  eh_host_port_reset_slave();
}

void C6Serial::send_(uint8_t op, const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len,
                     uint32_t checksum) {
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  const size_t total = head_len + data_len;
  const uint8_t header[8] = {0,
                             op,
                             static_cast<uint8_t>(total & 0xFF),
                             static_cast<uint8_t>(total >> 8),
                             static_cast<uint8_t>(checksum & 0xFF),
                             static_cast<uint8_t>((checksum >> 8) & 0xFF),
                             static_cast<uint8_t>((checksum >> 16) & 0xFF),
                             static_cast<uint8_t>(checksum >> 24)};
  uint8_t out[SLIP_CHUNK];
  size_t used = 0;
  auto put = [&](const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (used + 2 > sizeof(out)) {
        uart_write_bytes(port, out, used);
        used = 0;
      }
      if (bytes[i] == SLIP_END) {
        out[used++] = SLIP_ESC;
        out[used++] = SLIP_ESC_END;
      } else if (bytes[i] == SLIP_ESC) {
        out[used++] = SLIP_ESC;
        out[used++] = SLIP_ESC_ESC;
      } else {
        out[used++] = bytes[i];
      }
    }
  };
  out[used++] = SLIP_END;
  put(header, sizeof(header));
  put(head, head_len);
  put(data, data_len);
  if (used + 1 > sizeof(out)) {
    uart_write_bytes(port, out, used);
    used = 0;
  }
  out[used++] = SLIP_END;
  uart_write_bytes(port, out, used);
}

bool C6Serial::receive_(uint8_t op, uint32_t timeout_ms, uint8_t *out, size_t out_max, size_t *out_len) {
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  const uint32_t start = millis();
  uint8_t frame[FRAME_MAX];
  size_t len = 0;
  bool in_frame = false;
  bool escaped = false;
  for (;;) {
    const uint32_t spent = millis() - start;
    if (spent >= timeout_ms)
      return false;
    uint8_t byte;
    if (uart_read_bytes(port, &byte, 1, pdMS_TO_TICKS(timeout_ms - spent)) <= 0)
      return false;
    if (!in_frame) {
      if (byte == SLIP_END) {
        in_frame = true;
        len = 0;
        escaped = false;
      }
      continue;
    }
    if (byte == SLIP_END) {
      if (len >= 8 && frame[0] == 1 && frame[1] == op) {
        const size_t data_len = std::min<size_t>(len - 8, out_max);
        if (out != nullptr && data_len > 0)
          std::memcpy(out, frame + 8, data_len);
        if (out_len != nullptr)
          *out_len = data_len;
        return true;
      }
      len = 0;
      escaped = false;
      continue;
    }
    if (escaped) {
      byte = byte == SLIP_ESC_END ? SLIP_END : SLIP_ESC;
      escaped = false;
    } else if (byte == SLIP_ESC) {
      escaped = true;
      continue;
    }
    if (len < sizeof(frame))
      frame[len++] = byte;
  }
}

bool C6Serial::command_(uint8_t op, const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len,
                        uint32_t checksum, uint8_t *answer, size_t answer_len, uint32_t timeout_ms) {
  this->send_(op, head, head_len, data, data_len, checksum);
  uint8_t reply[FRAME_MAX];
  size_t reply_len = 0;
  if (!this->receive_(op, timeout_ms, reply, sizeof(reply), &reply_len)) {
    ESP_LOGW(TAG, "No answer to command 0x%02X", op);
    return false;
  }
  if (reply_len < answer_len + 2 || reply[answer_len] != 0) {
    ESP_LOGW(TAG, "Command 0x%02X refused (%u bytes, status 0x%02X, reason 0x%02X)", op,
             static_cast<unsigned>(reply_len), reply_len > answer_len ? reply[answer_len] : 0,
             reply_len > answer_len + 1 ? reply[answer_len + 1] : 0);
    return false;
  }
  if (answer != nullptr && answer_len > 0)
    std::memcpy(answer, reply, answer_len);
  return true;
}

bool C6Serial::sync_() {
  static const uint8_t HEAD[4] = {0x07, 0x07, 0x12, 0x20};
  uint8_t body[32];
  std::memset(body, 0x55, sizeof(body));
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  for (int attempt = 0; attempt < SYNC_ATTEMPTS; attempt++) {
    uart_flush_input(port);
    this->send_(OP_SYNC, HEAD, sizeof(HEAD), body, sizeof(body), 0);
    if (this->receive_(OP_SYNC, SYNC_TIMEOUT_MS, nullptr, 0, nullptr)) {
      vTaskDelay(pdMS_TO_TICKS(SYNC_SETTLE_MS));
      uart_flush_input(port);
      return true;
    }
  }
  return false;
}

bool C6Serial::write_block_(const uint8_t *block, uint32_t seq) {
  uint8_t head[16];
  put_u32(head, FLASH_BLOCK);
  put_u32(head + 4, seq);
  put_u32(head + 8, 0);
  put_u32(head + 12, 0);
  uint32_t checksum = CHECKSUM_START;
  for (uint32_t i = 0; i < FLASH_BLOCK; i++)
    checksum ^= block[i];
  for (int attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
    if (this->command_(OP_FLASH_DATA, head, sizeof(head), block, FLASH_BLOCK, checksum, nullptr, 0, COMMAND_TIMEOUT_MS))
      return true;
  }
  return false;
}

bool C6Serial::flash(const uint8_t *image, uint32_t size, SerialProgress progress, void *ctx) {
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  if (!this->sync_()) {
    ESP_LOGE(TAG, "The C6 bootloader does not answer");
    return false;
  }
  uint8_t params[24]{};
  if (!this->command_(OP_SPI_ATTACH, params, 8, nullptr, 0, 0, nullptr, 0, COMMAND_TIMEOUT_MS))
    return false;
  put_u32(params, 0);
  put_u32(params + 4, FLASH_LIMIT);
  put_u32(params + 8, FLASH_ERASE_BLOCK);
  put_u32(params + 12, FLASH_SECTOR);
  put_u32(params + 16, FLASH_PAGE);
  put_u32(params + 20, FLASH_STATUS_MASK);
  if (!this->command_(OP_SPI_SET_PARAMS, params, 24, nullptr, 0, 0, nullptr, 0, COMMAND_TIMEOUT_MS))
    return false;
  put_u32(params, this->baud_rate_);
  put_u32(params + 4, 0);
  this->send_(OP_CHANGE_BAUDRATE, params, 8, nullptr, 0, 0);
  if (!this->receive_(OP_CHANGE_BAUDRATE, COMMAND_TIMEOUT_MS, nullptr, 0, nullptr))
    return false;
  uart_set_baudrate(port, this->baud_rate_);
  vTaskDelay(pdMS_TO_TICKS(BAUD_SETTLE_MS));
  uart_flush_input(port);

  const uint32_t blocks = (size + FLASH_BLOCK - 1) / FLASH_BLOCK;
  put_u32(params, size);
  put_u32(params + 4, blocks);
  put_u32(params + 8, FLASH_BLOCK);
  put_u32(params + 12, 0);
  put_u32(params + 16, 0);
  ESP_LOGI(TAG, "Erasing %" PRIu32 " bytes", size);
  if (!this->command_(OP_FLASH_BEGIN, params, 20, nullptr, 0, 0, nullptr, 0, scaled_timeout(ERASE_MS_PER_MB, size)))
    return false;

  uint8_t last[FLASH_BLOCK];
  uint32_t reported = 0;
  for (uint32_t seq = 0; seq < blocks; seq++) {
    const uint32_t at = seq * FLASH_BLOCK;
    const uint32_t left = size - at;
    const uint8_t *block = image + at;
    if (left < FLASH_BLOCK) {
      std::memcpy(last, block, left);
      std::memset(last + left, 0xFF, FLASH_BLOCK - left);
      block = last;
    }
    if (!this->write_block_(block, seq)) {
      ESP_LOGE(TAG, "Writing failed at %" PRIu32, at);
      return false;
    }
    const uint32_t percent = (seq + 1) * 100 / blocks;
    if (percent >= reported + 5) {
      reported = percent;
      progress(ctx, static_cast<uint8_t>(percent));
    }
  }

  put_u32(params, 0);
  put_u32(params + 4, size);
  put_u32(params + 8, 0);
  put_u32(params + 12, 0);
  char answer[MD5_HEX_LEN + 1]{};
  if (!this->command_(OP_SPI_FLASH_MD5, params, 16, nullptr, 0, 0, reinterpret_cast<uint8_t *>(answer), MD5_HEX_LEN,
                      scaled_timeout(MD5_MS_PER_MB, size)))
    return false;
  md5::MD5Digest digest;
  digest.init();
  digest.add(image, size);
  digest.calculate();
  if (!digest.equals_hex(answer)) {
    ESP_LOGE(TAG, "MD5 of the written image differs");
    return false;
  }
  ESP_LOGI(TAG, "%" PRIu32 " bytes written and verified", size);
  return true;
}

void C6Serial::read_log(const std::atomic<bool> &stop) {
  const auto port = static_cast<uart_port_t>(this->uart_num_);
  char line[LOG_LINE];
  size_t len = 0;
  uint8_t chunk[64];
  while (!stop.load(std::memory_order_acquire)) {
    const int got = uart_read_bytes(port, chunk, sizeof(chunk), pdMS_TO_TICKS(LOG_READ_MS));
    for (int i = 0; i < got; i++) {
      const char c = static_cast<char>(chunk[i]);
      if (c == '\n' || c == '\r' || len == sizeof(line) - 1) {
        if (len > 0) {
          line[len] = '\0';
          ESP_LOGI(TAG, "C6 uart: %s", line);
          len = 0;
        }
      }
      if (c >= ' ' && c <= '~')
        line[len++] = c;
    }
  }
}

}

#endif
