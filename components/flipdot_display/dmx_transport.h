#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

#include <driver/uart.h>

#include "transport.h"

namespace esphome::flipdot_display {

/// DMX512 transport: hardware UART at 250 kbaud 8N2 through an external
/// RS-485 transceiver. One full universe frame per send() (start code 0x00 +
/// 512 slots), terminated by the trailing TX break the decoder latches on;
/// framing details are documented in flipdot-display.md.
class FlipdotDmxTransport : public FlipdotTransport {
 public:
  void setup() override;
  void dump_config() override;
  void send(const uint8_t *bytes, size_t len) override;

  void set_pin(uint8_t pin) { this->pin_number_ = pin; }
  void set_de_pin(uint8_t pin) { this->de_pin_number_ = pin; }

 protected:
  uint8_t pin_number_{0};
  uint8_t de_pin_number_{0};

  /// 1 start code byte + 512 slot bytes.
  uint8_t *frame_{nullptr};
  static constexpr uart_port_t UART_PORT = UART_NUM_1;
  static constexpr size_t FRAME_BYTES = 1 + 512;
  // Break length in bits at 250 kbaud: 120 us, DMX spec minimum is 88 us.
  static constexpr int BREAK_BITS = 30;
};

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
