#pragma once

#ifdef USE_ESP32

#include <vector>

#include "esphome/components/display/display_buffer.h"

#include "transport.h"

namespace esphome::flipdot_display {

/// Connection type (config: connection).
enum class ConnectionType : uint8_t {
  SINGLE_WIRE = 0,  ///< TM1824 chain driven directly on one data line (RMT)
  DMX = 1,          ///< DMX512 over RS-485 (UART + external transceiver)
};

/// Order of dots within a throttled transition (config: switching_effect).
enum class SwitchingEffect : uint8_t {
  NONE = 0,    ///< chain order (the order dots are wired in)
  WAVE = 1,    ///< Chebyshev distance from the display's top-right corner
  RANDOM = 2,  ///< shuffled on every transition
};

/// Monochrome display built from TM1824 PWM drivers connected in series.
///
/// One wire byte per dot; the same blit serves both transports, selected by
/// `connection`. The wire layout (mirrored Z within a block, blocks chained
/// as a snake) and the byte semantics are documented in flipdot-display.md
/// and the flipdot-stc8g HARDWARE.md.
class FlipdotDisplay : public display::DisplayBuffer {
 public:
  void set_transport(FlipdotTransport *transport) { this->transport_ = transport; }
  void set_connection(ConnectionType connection) { this->connection_ = connection; }
  /// DMX mode: base universe address of every block, in chain order.
  /// Absent/empty -> dense windows from address 1.
  void set_module_addresses(std::vector<uint16_t> addresses) { this->module_addr_ = std::move(addresses); }
  void set_grid(int cols, int rows, int block_width, int block_height);
  void set_on_level(uint8_t on_level) { this->on_level_ = on_level; }
  void set_off_level(uint8_t off_level) { this->off_level_ = off_level; }
  void set_concurrency(uint32_t concurrency) { this->concurrency_ = concurrency; }
  void set_step_interval(uint32_t step_ms) { this->step_interval_ms_ = step_ms; }
  void set_switching_effect(SwitchingEffect effect) { this->effect_ = effect; }

  void setup() override;
  void update() override;
  /// PollingComponent schedule plus the throttled-transition stepper.
  void loop() override;
  /// Map the frame buffer onto the wire bytes and transmit if anything changed.
  /// force=true writes the full frame regardless of changes.
  void display(bool force = false);
  void dump_config() override;

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

 protected:
  int get_width_internal() override { return this->width_px_; }
  int get_height_internal() override { return this->height_px_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  /// Map the frame buffer onto the wire bytes (the forward blit).
  void blit_words_();
  /// Diff the blit result against the committed bytes and start a throttled
  /// transition (all dots when force=true).
  void enqueue_transition_(bool force);
  /// Re-order the pending queue per the configured effect.
  void order_queue_();
  /// Send one batch of up to `concurrency` dots and commit their bytes.
  void step_transition_();
  /// Compose the transport frame from `bytes` and transmit exactly one frame.
  void send_bytes_(const uint8_t *bytes);

  FlipdotTransport *transport_{nullptr};
  ConnectionType connection_{ConnectionType::SINGLE_WIRE};
  /// DMX mode: base universe address per block (chain order). A block's
  /// bytes are copied into the universe at offset (addr - 1).
  std::vector<uint16_t> module_addr_;
  /// Composed universe payload for the DMX path (zeroed gaps between windows).
  uint8_t *dmx_frame_{nullptr};
  size_t dmx_len_{0};
  uint8_t on_level_{255};
  uint8_t off_level_{0};
  int cols_{0};
  int rows_{0};
  int block_width_{8};
  int block_height_{8};
  int width_px_{0};
  int height_px_{0};
  uint32_t total_pixels_{0};
  /// 1 byte per display pixel: 1 = on, 0 = off.
  uint8_t *pixels_{nullptr};
  /// Wire bytes in chain order, one byte per dot (exactly total_pixels_).
  uint8_t *words_{nullptr};
  /// The last transmitted wire bytes (chain order) — the dirty-tracking
  /// source and, in throttled mode, the base of every partial frame.
  uint8_t *committed_words_{nullptr};
  /// Effect applied to the transition queue.
  SwitchingEffect effect_{SwitchingEffect::NONE};
  /// Max dots switching in one step; 0 = a single frame per transition.
  uint32_t concurrency_{0};
  /// Pause between batches of a throttled transition.
  uint32_t step_interval_ms_{100};
  /// Pending transition: chain indices whose committed byte must change.
  std::vector<uint16_t> queue_;
  size_t queue_pos_{0};
  bool transition_active_{false};
  uint32_t last_step_ms_{0};
};

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
