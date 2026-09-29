#include "flipdot_display.h"

#include <cinttypes>

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

namespace esphome::flipdot_display {

static const char *const TAG = "flipdot_display";

static constexpr size_t DMX_UNIVERSE_SLOTS = 512;

void FlipdotDisplay::set_grid(int cols, int rows, int block_width, int block_height) {
  this->cols_ = cols;
  this->rows_ = rows;
  this->block_width_ = block_width;
  this->block_height_ = block_height;
  this->width_px_ = cols * block_width;
  this->height_px_ = rows * block_height;
  this->total_pixels_ = static_cast<uint32_t>(this->width_px_) * this->height_px_;
}

void FlipdotDisplay::setup() {
  if (this->transport_ == nullptr) {
    ESP_LOGE(TAG, "No transport is set");
    this->mark_failed();
    return;
  }

  RAMAllocator<uint8_t> allocator;
  this->pixels_ = allocator.allocate(this->total_pixels_);
  this->words_ = allocator.allocate(this->total_pixels_);
  this->committed_words_ = allocator.allocate(this->total_pixels_);
  if (this->connection_ == ConnectionType::DMX) {
    const int blocks = this->cols_ * this->rows_;
    if (static_cast<int>(this->module_addr_.size()) != blocks) {
      ESP_LOGE(TAG, "Module address count mismatch: %zu, expected %d", this->module_addr_.size(), blocks);
      this->mark_failed();
      return;
    }
    const int block_pixels = this->block_width_ * this->block_height_;
    for (uint16_t addr : this->module_addr_) {
      const size_t need = static_cast<size_t>(addr) + block_pixels - 1;
      if (need > this->dmx_len_)
        this->dmx_len_ = need;
    }
    if (this->dmx_len_ > DMX_UNIVERSE_SLOTS) {
      ESP_LOGE(TAG, "DMX universe too small for the module windows: %zu needed, %u available", this->dmx_len_,
               (unsigned) DMX_UNIVERSE_SLOTS);
      this->mark_failed();
      return;
    }
    this->dmx_frame_ = allocator.allocate(this->dmx_len_);
  }
  if (this->pixels_ == nullptr || this->words_ == nullptr || this->committed_words_ == nullptr ||
      (this->connection_ == ConnectionType::DMX && this->dmx_frame_ == nullptr)) {
    ESP_LOGE(TAG, "Cannot allocate display buffers!");
    this->mark_failed();
    return;
  }
  memset(this->pixels_, 0, this->total_pixels_);
  memset(this->words_, 0, this->total_pixels_);
  memset(this->committed_words_, 0, this->total_pixels_);
  if (this->dmx_frame_ != nullptr)
    memset(this->dmx_frame_, 0, this->dmx_len_);

  this->transport_->setup();
  if (this->transport_->is_failed())
    this->mark_failed();
}

void FlipdotDisplay::update() {
  this->do_update_();
  this->display();
}

void FlipdotDisplay::loop() {
  this->PollingComponent::loop();
  if (!this->transition_active_)
    return;
  const uint32_t now = millis();
  if (now - this->last_step_ms_ < this->step_interval_ms_)
    return;
  this->step_transition_();
}

void HOT FlipdotDisplay::display(bool force) {
  if (this->pixels_ == nullptr || this->words_ == nullptr || this->committed_words_ == nullptr)
    return;
  if (this->transport_ == nullptr || this->transport_->is_failed())
    return;

  this->blit_words_();

  if (this->concurrency_ == 0) {
    // Classic path: the whole transition is one frame.
    const size_t wire_len = this->total_pixels_;
    bool dirty = force;
    if (!dirty) {
      for (size_t i = 0; i < wire_len; i++) {
        if (this->words_[i] != this->committed_words_[i]) {
          dirty = true;
          break;
        }
      }
    }
    if (dirty) {
      this->send_bytes_(this->words_);
      memcpy(this->committed_words_, this->words_, wire_len);
    }
    return;
  }

  this->enqueue_transition_(force);
}

void FlipdotDisplay::blit_words_() {
  // Forward blit: for every display pixel, place its byte at its chain
  // position (mirrored Z within a block, blocks chained as a snake):
  //   even block_y: g = (block_y * cols + (cols - 1 - block_x)) * block_pixels
  //   odd block_y:  g = (block_y * cols + block_x) * block_pixels
  //       + in_y * block_width + (block_width - 1 - in_x)
  const int bw = this->block_width_;
  const int bh = this->block_height_;
  const int block_pixels = bw * bh;
  for (int y = 0; y < this->height_px_; y++) {
    const int block_y = y / bh;
    const int in_y = y % bh;
    const int block_row_base = block_y * this->cols_ * block_pixels + in_y * bw;
    const bool row_reversed = block_y % 2 == 0;
    const uint8_t *row = this->pixels_ + static_cast<uint32_t>(y) * this->width_px_;
    for (int x = 0; x < this->width_px_; x++) {
      const int bx = x / bw;
      const int block = block_row_base + (row_reversed ? this->cols_ - 1 - bx : bx) * block_pixels;
      const int g = block + (bw - 1 - (x % bw));
      this->words_[g] = row[x] ? this->on_level_ : this->off_level_;
    }
  }
}

void FlipdotDisplay::enqueue_transition_(bool force) {
  const size_t wire_len = this->total_pixels_;
  this->queue_.clear();
  this->queue_pos_ = 0;
  for (size_t g = 0; g < wire_len; g++)
    if (force || this->words_[g] != this->committed_words_[g])
      this->queue_.push_back(static_cast<uint16_t>(g));
  if (this->queue_.empty()) {
    // The target caught up with the committed bytes — nothing pending.
    // Deactivate instead of leaving a zombie transition that would send one
    // redundant frame on the next loop step.
    this->transition_active_ = false;
    return;
  }
  const bool was_active = this->transition_active_;
  this->order_queue_();
  this->transition_active_ = true;
  if (!was_active) {
    // New transition: fire the first batch right away; an already-active
    // transition keeps its step_interval cadence (update() only refreshed
    // the target — see flipdot-display.md "Throttled switching").
    const uint32_t batch = this->concurrency_ ? this->concurrency_ : this->queue_.size();
    ESP_LOGD(TAG, "Transition: %u dot(s), %u batch(es)", (unsigned) this->queue_.size(),
             (unsigned) ((this->queue_.size() + batch - 1) / batch));
    this->last_step_ms_ = 0;
    this->step_transition_();
  }
}

void FlipdotDisplay::order_queue_() {
  switch (this->effect_) {
    case SwitchingEffect::RANDOM: {
      static std::mt19937 rng(std::random_device{}());
      std::shuffle(this->queue_.begin(), this->queue_.end(), rng);
      return;
    }
    case SwitchingEffect::WAVE: {
      // Sort by Chebyshev distance from the display's top-right corner
      // (x = width-1, y = 0); ties keep the chain order. The blit inverse:
      //   block k -> block_y = k / cols; even row: block_x = cols - 1 - k % cols,
      //   odd row: block_x = k % cols (the block rows chain as a snake)
      //   chain offset o within the block -> (in_y = o / bw, in_x = bw - 1 - o % bw)
      const int bp = this->block_width_ * this->block_height_;
      std::vector<std::pair<uint32_t, uint16_t>> items;
      items.reserve(this->queue_.size());
      for (uint16_t g : this->queue_) {
        const int block = g / bp;
        const int o = g % bp;
        const int block_y = block / this->cols_;
        const int kx = block % this->cols_;
        const int block_x = (block_y % 2 == 0) ? this->cols_ - 1 - kx : kx;
        const int in_y = o / this->block_width_;
        const int in_x = this->block_width_ - 1 - o % this->block_width_;
        const int dx = this->width_px_ - 1 - (block_x * this->block_width_ + in_x);
        const int dy = block_y * this->block_height_ + in_y;
        items.emplace_back(std::max(dx, dy), g);
      }
      std::sort(items.begin(), items.end());
      for (size_t i = 0; i < items.size(); i++)
        this->queue_[i] = items[i].second;
      return;
    }
    case SwitchingEffect::NONE:
    default:
      return;
  }
}

void FlipdotDisplay::step_transition_() {
  const uint32_t now = millis();
  uint32_t batch = this->concurrency_;
  if (batch == 0)
    batch = static_cast<uint32_t>(this->queue_.size());
  uint32_t done = 0;
  while (this->queue_pos_ < this->queue_.size() && done < batch) {
    const uint16_t g = this->queue_[this->queue_pos_++];
    this->committed_words_[g] = this->words_[g];
    done++;
  }
  ESP_LOGV(TAG, "Batch: %u dot(s), %u left", (unsigned) done, (unsigned) (this->queue_.size() - this->queue_pos_));
  if (this->queue_pos_ >= this->queue_.size()) {
    this->queue_pos_ = 0;
    this->transition_active_ = false;
    ESP_LOGD(TAG, "Transition done");
  }

  this->send_bytes_(this->committed_words_);
  this->last_step_ms_ = now;
}

void FlipdotDisplay::send_bytes_(const uint8_t *bytes) {
  if (this->connection_ == ConnectionType::DMX) {
    // gaps between module windows stay 0x00 (release)
    const size_t block_pixels = static_cast<size_t>(this->block_width_) * this->block_height_;
    memset(this->dmx_frame_, 0, this->dmx_len_);
    for (size_t k = 0; k < this->module_addr_.size(); k++)
      memcpy(this->dmx_frame_ + (this->module_addr_[k] - 1), bytes + k * block_pixels, block_pixels);
    this->transport_->send(this->dmx_frame_, this->dmx_len_);
  } else {
    this->transport_->send(bytes, this->total_pixels_);
  }
}

void HOT FlipdotDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= this->width_px_ || y < 0 || y >= this->height_px_)
    return;
  const bool on = color.r != 0 || color.g != 0 || color.b != 0 || color.w != 0;
  this->pixels_[static_cast<uint32_t>(y) * this->width_px_ + x] = on ? 1 : 0;
}

void FlipdotDisplay::dump_config() {
  LOG_DISPLAY("", "Flipdot Display", this);
  ESP_LOGCONFIG(TAG, "  Blocks: %dx%d of %dx%d px", this->cols_, this->rows_, this->block_width_, this->block_height_);
  ESP_LOGCONFIG(TAG, "  Chain bytes: %" PRIu32, this->total_pixels_);
  ESP_LOGCONFIG(TAG, "  On level: %u", this->on_level_);
  ESP_LOGCONFIG(TAG, "  Off level: %u", this->off_level_);
  ESP_LOGCONFIG(TAG, "  Concurrency: %" PRIu32, this->concurrency_);
  if (this->concurrency_ > 0) {
    ESP_LOGCONFIG(TAG, "  Step interval: %" PRIu32 " ms", this->step_interval_ms_);
    const char *effect = "none";
    if (this->effect_ == SwitchingEffect::WAVE)
      effect = "wave";
    else if (this->effect_ == SwitchingEffect::RANDOM)
      effect = "random";
    ESP_LOGCONFIG(TAG, "  Switching effect: %s", effect);
  }
  if (this->connection_ == ConnectionType::DMX) {
    ESP_LOGCONFIG(TAG, "  Connection: DMX512 over RS-485");
    for (uint16_t addr : this->module_addr_)
      ESP_LOGCONFIG(TAG, "  Module base address: %u", addr);
  } else {
    ESP_LOGCONFIG(TAG, "  Connection: single-wire TM1824 chain");
  }
  if (this->transport_ != nullptr)
    this->transport_->dump_config();
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
