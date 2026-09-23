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

void FlipdotDmxTransport::debug_long_break(uint32_t ms) {
  if (this->failed_ || this->frame_ == nullptr)
    return;
  // 255 bit times is the IDF break cap; pick the baud rate so one break
  // covers the requested hold, floor at the minimum runnable baud on the
  // 80 MHz clock (1220). Effective duration shortens below that.
  const uint32_t baud = (255000u + ms - 1) / ms;
  const uint32_t eff_baud = (baud < 1220) ? 1220 : baud;
  uart_set_baudrate(UART_PORT, eff_baud);
  // one junk 0x00 byte (an empty start code 0x00 frame for the decoder)
  // followed by the hardware break at the lowered baud rate
  const uint8_t dummy = 0x00;
  uart_write_bytes_with_break(UART_PORT, &dummy, 1, 255);
  uart_set_baudrate(UART_PORT, DMX_BAUD);
  ESP_LOGD(TAG, "Long break: %u ms (baud %u, effective %.1f ms)", ms, eff_baud, 255000.0 / eff_baud);
}

void FlipdotDmxTransport::de_level_(uint8_t level) {
  if (this->failed_)
    return;
  gpio_set_level(gpio_num_t(this->de_pin_number_), level);
  ESP_LOGD(TAG, "Transceiver %s", level ? "driving" : "released (floating bus)");
}

}  // namespace esphome::flipdot_display

#endif  // USE_ESP32
