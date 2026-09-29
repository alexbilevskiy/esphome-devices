#pragma once

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>

#include <driver/rmt_tx.h>

#include "transport.h"

namespace esphome::flipdot_display {

struct RmtParams {
  rmt_symbol_word_t bit0;
  rmt_symbol_word_t bit1;
  rmt_symbol_word_t reset;
};

/// Single-wire transport: drives the TM1824 chain directly from an RMT
/// channel. One byte per dot encoded MSB-first, eight RMT symbols per byte;
/// the frame terminates in the low reset that latches the chain (bit timings
/// in the .cpp, per the TM1824 datasheet). The line parks at eot_level after
/// the frame; RMT does not repeat frames.
class FlipdotRmtTransport : public FlipdotTransport {
 public:
  void setup() override;
  void dump_config() override;
  void send(const uint8_t *bytes, size_t len) override;

  void set_pin(uint8_t pin) { this->pin_number_ = pin; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void set_eot_level(uint8_t eot_level) { this->eot_level_ = eot_level; }
  void set_rmt_symbols(uint32_t rmt_symbols) { this->rmt_symbols_ = rmt_symbols; }
  /// Wire frame length in bytes (one byte per dot = the display's total pixel
  /// count). Must be set before setup().
  void set_frame_size(size_t frame_size) { this->frame_size_ = frame_size; }

 protected:
  uint8_t pin_number_{0};
  bool inverted_{false};
  uint8_t eot_level_{0};
  uint32_t rmt_symbols_{192};
  size_t frame_size_{0};

  RmtParams params_{};
  uint8_t *buf_{nullptr};
  rmt_channel_handle_t channel_{nullptr};
  rmt_encoder_handle_t frame_encoder_{nullptr};
  bool enabled_{false};
};

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
