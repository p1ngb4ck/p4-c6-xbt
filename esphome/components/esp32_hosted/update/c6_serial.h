#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_HOSTED_SERIAL

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome::esp32_hosted {

using SerialProgress = void (*)(void *ctx, uint8_t percent);

class C6Serial {
 public:
  void set_config(uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin, uint32_t baud_rate);
  void set_pins(uint8_t uart_num, int tx_pin, int rx_pin, int boot_pin);
  static bool image_ok(const uint8_t *image, uint32_t size);
  bool open();
  void close();
  void enter_bootloader();
  void leave_bootloader();
  bool flash(const uint8_t *image, uint32_t size, SerialProgress progress, void *ctx);
  void read_log(const std::atomic<bool> &stop);

 protected:
  void send_(uint8_t op, const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len, uint32_t checksum);
  bool receive_(uint8_t op, uint32_t timeout_ms, uint8_t *out, size_t out_max, size_t *out_len);
  bool command_(uint8_t op, const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len,
                uint32_t checksum, uint8_t *answer, size_t answer_len, uint32_t timeout_ms);
  bool sync_();
  bool write_block_(const uint8_t *block, uint32_t seq);

  uint8_t uart_num_{0};
  int tx_pin_{-1};
  int rx_pin_{-1};
  int boot_pin_{-1};
  uint32_t baud_rate_{460800};
  uint8_t open_uart_{0};
  int open_tx_{-1};
  int open_rx_{-1};
};

}

#endif
