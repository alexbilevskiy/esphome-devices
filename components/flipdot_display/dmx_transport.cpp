#include "dmx_transport.h"

#include <cinttypes>
#include <cstring>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>

#ifdef USE_ESP32

namespace esphome::flipdot_display {

static const char *const TAG = "flipdot_dmx";

static const int DMX_BAUD = 250000;

void FlipdotDmxTransport::setup() {
  // sole master on the bus: keep the transceiver driving all the time
  gpio_set_direction(gpio_num_t(this->de_pin_number_), GPIO_MODE_OUTPUT);
  gpio_set_level(gpio_num_t(this->de_pin_number_), 1);

  RAMAllocator<uint8_t> allocator;
  this->frame_ = allocator.allocate(FRAME_BYTES);
  if (this->frame_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate frame buffer!");
    this->mark_failed();
    return;
  }
  memset(this->frame_, 0, FRAME_BYTES);

  uart_config_t config{};
  config.baud_rate = DMX_BAUD;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_2;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  if (uart_param_config(UART_PORT, &config) != ESP_OK) {
    ESP_LOGE(TAG, "UART param config failed");
    this->mark_failed();
    return;
  }
  if (uart_set_pin(UART_PORT, this->pin_number_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
      ESP_OK) {
    ESP_LOGE(TAG, "UART pin mux failed");
    this->mark_failed();
    return;
  }
  // RX unused (must still exceed the HW FIFO length), TX buffered: the frame
  // fits in the ring buffer, the break waits for TX_BRK_DONE on the driver side
  if (uart_driver_install(UART_PORT, 256, FRAME_BYTES * 2, 0, nullptr, 0) != ESP_OK) {
    ESP_LOGE(TAG, "UART driver install failed");
    this->mark_failed();
    return;
  }
}

void FlipdotDmxTransport::dump_config() {
  ESP_LOGCONFIG(TAG, "DMX512 transport:");
  ESP_LOGCONFIG(TAG, "  Pin: %u", this->pin_number_);
  ESP_LOGCONFIG(TAG, "  DE Pin: %u", this->de_pin_number_);
  ESP_LOGCONFIG(TAG, "  Universe: start code 0x00 + %u slots", (unsigned) (FRAME_BYTES - 1));
}

void FlipdotDmxTransport::send(const uint8_t *bytes, size_t len) {
  if (this->failed_ || this->frame_ == nullptr)
    return;
  if (len > FRAME_BYTES - 1)
    len = FRAME_BYTES - 1;
  // start code + all-zero tail, payload at universe slots 1..len
  memset(this->frame_, 0, FRAME_BYTES);
  if (len > 0)
    memcpy(this->frame_ + 1, bytes, len);

  // the call blocks for the whole frame (~24 ms) and takes the tx_mux; the
  // break is appended after the payload and waits for TX_BRK_DONE
  const size_t written = uart_write_bytes_with_break(UART_PORT, this->frame_, FRAME_BYTES, BREAK_BITS);
  if (written != FRAME_BYTES)
    ESP_LOGE(TAG, "UART TX error: wrote %" PRIu32 " of %" PRIu32, (uint32_t) written, (uint32_t) FRAME_BYTES);
  else
    ESP_LOGD(TAG, "Sent DMX frame (%zu bytes window payload)", len);
}

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
