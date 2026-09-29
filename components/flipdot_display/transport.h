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

 protected:
  void mark_failed() { this->failed_ = true; }
  bool failed_{false};
};

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
