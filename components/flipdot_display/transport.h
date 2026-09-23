#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

namespace esphome::flipdot_display {

/// Wire transmitter abstraction for the flipdot display. A transport owns the
/// peripheral (RMT channel or UART) and carries exactly one frame per send().
/// Transports are not Components: the display drives setup()/dump_config(), so
/// the whole pipeline stays a single YAML entity.
class FlipdotTransport {
 public:
  virtual ~FlipdotTransport() = default;

  virtual void setup() = 0;
  virtual void dump_config() = 0;
  /// Transmit exactly one frame carrying the first `len` bytes of `bytes`.
  virtual void send(const uint8_t *bytes, size_t len) = 0;

  bool is_failed() const { return this->failed_; }

  /// Debug probes for the bench rig. The no-op defaults belong to transports
  /// without the feature; the display guards by connection type and logs the
  /// mismatch itself, so these are only called on supporting transports.
  virtual void debug_long_break(uint32_t /*ms*/) {}
  virtual void debug_bus_off() {}
  virtual void debug_bus_on() {}
  virtual void debug_pin_low() {}
  virtual void debug_pin_high() {}

 protected:
  void mark_failed() { this->failed_ = true; }
  bool failed_{false};
};

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
