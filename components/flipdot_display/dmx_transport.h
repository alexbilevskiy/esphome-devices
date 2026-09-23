#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

#include <driver/uart.h>

#include "transport.h"

namespace esphome::flipdot_display {

/// DMX512 transport: hardware UART at 250 kbaud 8N2 through an external
/// RS-485 transceiver (MAX3485 class): ESP pin -> transceiver DI, DE pin ->
/// transceiver DE, transceiver A/B -> module A/B, common ground.
///
/// Wire format: every send() transmits one full universe frame (start code
/// 0x00 + 512 data slots) terminating in a RMT-style TX break. The IDF uart
/// driver sends the break AFTER the payload, which is exactly how a DMX
/// decoder frames data: slots are latched when the trailing break arrives,
/// and the idle time until the next frame is the mark-after-break. The
/// payload's channel i maps to universe slot 1 + i (DMX slots are 1-based;
/// slot 0 is the alternating start code).
class FlipdotDmxTransport : public FlipdotTransport {
  // DE (driver enable) of the external RS-485 transceiver is held high all
  // the time: this device is the only transmitter on the bus, and while the
  // UART idles its TX mark keeps the bus in the DMX idle state.
 public:
  void setup() override;
  void dump_config() override;
  void send(const uint8_t *bytes, size_t len) override;

  /// Signal-loss probe: send a break far longer than the DMX minimum.
  void debug_long_break(uint32_t ms) override;
  /// Signal-loss probes: release/restore the transceiver output; a floating
  /// bus is the closest emulation of the cable being pulled.
  void debug_bus_off() override { this->de_level_(0); }
  void debug_bus_on() override { this->de_level_(1); }

  void set_pin(uint8_t pin) { this->pin_number_ = pin; }
  void set_de_pin(uint8_t pin) { this->de_pin_number_ = pin; }

 protected:
  /// Drive the transceiver enable line by hand.
  void de_level_(uint8_t level);

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
