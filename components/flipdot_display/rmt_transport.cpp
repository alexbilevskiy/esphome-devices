#include "rmt_transport.h"

#include <cinttypes>
#include <cstring>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_clk_tree.h>

#ifdef USE_ESP32

namespace esphome::flipdot_display {

static const char *const TAG = "flipdot_rmt";

static const size_t RMT_SYMBOLS_PER_BYTE = 8;

// Bit timings in nanoseconds, per the TM1824 datasheet V1.2 (Titan Micro):
// T0h 310..410 ns (typ 360), T1h 650..1000 ns (typ 720), bit period
// 1.25..2.5 us (we use the 800 kHz nominal 1.25 us), reset low >= 200 us.
static const uint32_t BIT0_HIGH_NS = 360;
static const uint32_t BIT0_LOW_NS = 890;
static const uint32_t BIT1_HIGH_NS = 720;
static const uint32_t BIT1_LOW_NS = 530;
static const uint32_t RESET_HIGH_NS = 0;
static const uint32_t RESET_LOW_NS = 300000;

// Query the RMT default clock source frequency. This varies by variant:
// APB (80MHz) on ESP32/S2/S3/C3, PLL_F80M (80MHz) on C6/P4, XTAL (32MHz) on H2.
static uint32_t rmt_resolution_hz() {
  uint32_t freq;
  esp_clk_tree_src_get_freq_hz((soc_module_clk_t) RMT_CLK_SRC_DEFAULT, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &freq);
  return freq;
}

static size_t IRAM_ATTR HOT frame_encoder_callback(const void *data, size_t size, size_t symbols_written,
                                                   size_t symbols_free, rmt_symbol_word_t *symbols, bool *done,
                                                   void *arg) {
  auto *params = static_cast<RmtParams *>(arg);
  const auto *bytes = static_cast<const uint8_t *>(data);
  const size_t index = symbols_written / RMT_SYMBOLS_PER_BYTE;

  // convert one byte to eight symbols
  if (index < size) {
    if (symbols_free < RMT_SYMBOLS_PER_BYTE)
      return 0;
    for (size_t i = 0; i < RMT_SYMBOLS_PER_BYTE; i++)
      symbols[i] = (bytes[index] & (1 << (7 - i))) ? params->bit1 : params->bit0;
    return RMT_SYMBOLS_PER_BYTE;
  }

  // send the reset symbol (300 us low) once the frame is out
  if (symbols_free < 1)
    return 0;
  symbols[0] = params->reset;
  *done = true;
  return 1;
}

void FlipdotRmtTransport::setup() {
  if (this->frame_size_ == 0) {
    ESP_LOGE(TAG, "Frame size is not set");
    this->mark_failed();
    return;
  }
  // drive the line low while the RMT channel is not enabled yet
  gpio_set_direction(gpio_num_t(this->pin_number_), GPIO_MODE_OUTPUT);
  gpio_set_level(gpio_num_t(this->pin_number_), 0);

  RAMAllocator<uint8_t> allocator;
  this->buf_ = allocator.allocate(this->frame_size_);
  if (this->buf_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate frame buffer!");
    this->mark_failed();
    return;
  }
  memset(this->buf_, 0, this->frame_size_);

  const float ratio = (float) rmt_resolution_hz() / 1e09f;
  // 0-bit
  this->params_.bit0.duration0 = (uint32_t) (ratio * BIT0_HIGH_NS);
  this->params_.bit0.level0 = 1;
  this->params_.bit0.duration1 = (uint32_t) (ratio * BIT0_LOW_NS);
  this->params_.bit0.level1 = 0;
  // 1-bit
  this->params_.bit1.duration0 = (uint32_t) (ratio * BIT1_HIGH_NS);
  this->params_.bit1.level0 = 1;
  this->params_.bit1.duration1 = (uint32_t) (ratio * BIT1_LOW_NS);
  this->params_.bit1.level1 = 0;
  // reset
  this->params_.reset.duration0 = (uint32_t) (ratio * RESET_HIGH_NS);
  this->params_.reset.level0 = 1;
  this->params_.reset.duration1 = (uint32_t) (ratio * RESET_LOW_NS);
  this->params_.reset.level1 = 0;

  rmt_tx_channel_config_t channel;
  memset(&channel, 0, sizeof(channel));
  channel.clk_src = RMT_CLK_SRC_DEFAULT;
  channel.resolution_hz = rmt_resolution_hz();
  channel.gpio_num = gpio_num_t(this->pin_number_);
  channel.mem_block_symbols = this->rmt_symbols_;
  channel.trans_queue_depth = 1;
  channel.intr_priority = 0;
  channel.flags.invert_out = this->inverted_ ? 1 : 0;
  if (rmt_new_tx_channel(&channel, &this->channel_) != ESP_OK) {
    ESP_LOGE(TAG, "Channel creation failed");
    this->mark_failed();
    return;
  }

  rmt_simple_encoder_config_t frame_encoder;
  memset(&frame_encoder, 0, sizeof(frame_encoder));
  frame_encoder.callback = frame_encoder_callback;
  frame_encoder.arg = &this->params_;
  frame_encoder.min_chunk_size = RMT_SYMBOLS_PER_BYTE;
  if (rmt_new_simple_encoder(&frame_encoder, &this->frame_encoder_) != ESP_OK) {
    ESP_LOGE(TAG, "Frame encoder creation failed");
    this->mark_failed();
    return;
  }
}

void FlipdotRmtTransport::dump_config() {
  ESP_LOGCONFIG(TAG, "Single-wire (RMT) transport:");
  ESP_LOGCONFIG(TAG, "  Pin: %u", this->pin_number_);
  ESP_LOGCONFIG(TAG, "  Inverted: %s", YESNO(this->inverted_));
  ESP_LOGCONFIG(TAG, "  Frame bytes: %zu", this->frame_size_);
  ESP_LOGCONFIG(TAG, "  EOT level: %u", this->eot_level_);
  ESP_LOGCONFIG(TAG, "  RMT symbols: %" PRIu32, this->rmt_symbols_);
}

void FlipdotRmtTransport::send(const uint8_t *bytes, size_t len) {
  if (this->failed_ || this->channel_ == nullptr || this->buf_ == nullptr)
    return;
  if (!this->enabled_ && rmt_enable(this->channel_) != ESP_OK) {
    ESP_LOGE(TAG, "Enabling channel failed");
    return;
  }
  this->enabled_ = true;

  if (rmt_tx_wait_all_done(this->channel_, 1000) != ESP_OK) {
    ESP_LOGW(TAG, "RMT TX timeout waiting for the previous frame");
    return;
  }
  if (len > this->frame_size_) {
    ESP_LOGE(TAG, "Frame too large: %zu, buffer is %zu", len, this->frame_size_);
    return;
  }

  // copy into the transport's own buffer first: the transmission is async and
  // the encoder keeps reading the buffer until the frame is out, while the
  // caller may already re-blit its bytes for the next frame
  memcpy(this->buf_, bytes, len);
  rmt_transmit_config_t config;
  memset(&config, 0, sizeof(config));
  config.flags.eot_level = this->eot_level_;
  if (rmt_transmit(this->channel_, this->frame_encoder_, this->buf_, len, &config) != ESP_OK) {
    ESP_LOGE(TAG, "RMT TX error");
    return;
  }
  ESP_LOGD(TAG, "Sent one frame: %zu bytes", len);
}

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
