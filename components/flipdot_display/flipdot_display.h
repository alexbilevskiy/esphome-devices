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

/// Monochrome display built from daisy-chained TM1824 PWM drivers.
///
/// In a module the dots are wired in the same order on both buses: chain order
/// on the single-wire line equals the DMX window order (one dot per slot at
/// the module's base address). The same blit therefore serves both
/// transports, selected by `connection`:
///   - single_wire: the RMT transport drives the chain directly (one wire
///     byte per dot, MSB first, reset latch at the end of the frame),
///   - dmx: the DMX512 transport drives the RS-485 bus (one byte per dot;
///     block k (chain order) at its module's base address, slots
///     addr..addr+63).
///
/// Within a block, pixels are wired as a mirrored Z: every row is scanned
/// right-to-left, pixel 0 of a block is its top-right corner and its last
/// pixel is the bottom-left corner. Blocks are chained as a snake:
/// block row 0 right-to-left, row 1 left-to-right, row 2 right-to-left again,
/// top-to-bottom across block rows.
class FlipdotDisplay : public display::DisplayBuffer {
 public:
  void set_transport(FlipdotTransport *transport) { this->transport_ = transport; }
  void set_connection(ConnectionType connection) { this->connection_ = connection; }
  /// DMX mode: base universe address of every block, in chain order (the
  /// top-right block of row 0 first; the chain snakes across block rows).
  /// Block k occupies slots addr..addr+block_pixels-1;
  /// a module's decoder reads exactly that window. Absent/empty -> dense
  /// windows from address 1.
  void set_module_addresses(std::vector<uint16_t> addresses) { this->module_addr_ = std::move(addresses); }
  void set_grid(int cols, int rows, int block_width, int block_height);
  void set_on_level(uint8_t on_level) { this->on_level_ = on_level; }
  void set_off_level(uint8_t off_level) { this->off_level_ = off_level; }
  void set_concurrency(uint32_t concurrency) { this->concurrency_ = concurrency; }
  void set_step_interval(uint32_t step_ms) { this->step_interval_ms_ = step_ms; }
  void set_switching_effect(SwitchingEffect effect) { this->effect_ = effect; }

  void setup() override;
  void update() override;
  /// PollingComponent schedule plus the transition stepper: while a throttled
  /// transition is active, one batch of at most `concurrency` dots is sent
  /// per `step_interval`. Each step is one frame, so the loop never blocks for
  /// more than a single transmission.
  void loop() override;
  /// Map the frame buffer onto the wire bytes and transmit if anything changed.
  /// force=true writes the full frame regardless of changes.
  void display(bool force = false);
  void dump_config() override;

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

  /// Debug probe: transmit one frame with every wire byte set to `value` and
  /// sync the committed state, so dirty tracking stays correct afterwards.
  /// Cancels an in-flight throttled transition. In DMX mode the uniform bytes
  /// land in the module windows; gaps between windows stay at the release
  /// byte (0x00).
  void debug_uniform_frame(uint8_t value);
  /// Debug probes forwarded to the transport; each logs an error when the
  /// active connection does not support it.
  void debug_long_break(uint32_t ms);
  void debug_bus_off();
  void debug_bus_on();
  void debug_pin_low();
  void debug_pin_high();

 protected:
  int get_width_internal() override { return this->width_px_; }
  int get_height_internal() override { return this->height_px_; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  /// Map the frame buffer onto the wire bytes (the forward blit).
  void blit_words_();
  /// Diff the blit result against the committed bytes, order the changed dots
  /// per the switching effect and start a throttled transition. force=true
  /// enqueues every dot regardless of changes.
  void enqueue_transition_(bool force);
  /// Re-order the pending queue per the configured effect.
  void order_queue_();
  /// Send one batch of up to `concurrency` dots and commit their bytes.
  void step_transition_();
  /// Compose the transport frame from `bytes` and transmit exactly one frame:
  /// DMX mode composes the universe payload (zeroed gaps between module
  /// windows); single-wire mode sends the chain bytes directly.
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
  /// In DMX mode each block's bytes land at its module's base address.
  uint8_t *words_{nullptr};
  /// The last transmitted wire bytes (chain order) — the state dots are aware
  /// of being latched in. Used for dirty tracking and, in throttled mode, as
  /// the source of every partial frame. Allocated for both transmitters.
  uint8_t *committed_words_{nullptr};
  /// Effect applied to the transition queue.
  SwitchingEffect effect_{SwitchingEffect::NONE};
  /// Max dots switching in one step; 0 = all dots of a transition switch in a
  /// single frame (the classic behavior).
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
