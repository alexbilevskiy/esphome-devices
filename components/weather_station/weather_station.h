#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/display/display.h"
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

namespace esphome::weather_station {

struct Particle {
  float x;
  float y;
  uint32_t timer;
  uint8_t r, g, b;
  const char *type;
  float delay;
  float h_accum;
};

struct Pixel {
  int16_t x;
  int16_t y;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

class WeatherStation : public Component {
 public:
  void setup() override;
  void loop() override;

  void set_panel_size(int w, int h) {
    this->panel_w_ = w;
    this->panel_h_ = h;
  }

  void set_display(display::Display *display) { this->display_ = display; }

  void set_simulate_precip(const std::string &v) { this->simulate_precip_ = v; }
  void set_simulate_precip_strength(float v) { this->simulate_precip_strength_ = v; }
  void set_simulate_wind_speed(float v) { this->simulate_wind_speed_ = v; }

  void set_precip_type(int v) { this->prec_type_ = v; }
  void set_precip_strength(float v) { this->prec_strength_ = v; }
  void set_wind_speed_real(int v) { this->wind_speed_ = v; }

  void set_sun_rising(const std::string &v) { this->sun_rising_ = v; }
  void set_sun_setting(const std::string &v) { this->sun_setting_ = v; }
  void set_moon_rising(const std::string &v) { this->moon_rising_ = v; }
  void set_moon_setting(const std::string &v) { this->moon_setting_ = v; }

  // Called from the LVGL on_render_ready trigger after a real render: the LVGL
  // draw buffer now holds the composited UI frame. Copy it as the background
  // snapshot, then redraw the current overlay pixels (the flush overwrote them
  // in the dirty areas). With full_refresh: true the buffer is always the full
  // screen; a mismatched w/h invalidates the snapshot instead of corrupting it.
  void on_frame_composited(const uint8_t *data, uint32_t stride_bytes, uint32_t buf_w, uint32_t buf_h);

  // Paced render tick rate (EMA), ~50 when running at the 20ms cadence.
  float get_render_fps() const { return this->render_fps_ema_; }

  // Legacy pixel source for the YAML-canvas rendering approach (used by the
  // esp32-hub75.yaml config). Not used by the direct-to-framebuffer path.
  const std::vector<Pixel> &get_pixels() const { return this->pixels_; }

  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void render_();
  void refresh_sky_cache_();
  void draw_pixel_raw_(int x, int y, uint16_t color565);
  void update_particles_();
  void update_sky_border_();
  void sky_position_(int64_t rise, int64_t set, int64_t now, int &x, int &y, bool &visible);
  void sky_neighbors_(int x, int y, int out[][2], int &count);
  void get_color_for_precip_(const char *type, uint8_t &r, uint8_t &g, uint8_t &b);
  void angle_to_border_(float angle, int &x, int &y);
  void border_neighbors_(int x, int y, int out[][2], int &count);
  int64_t parse_iso_datetime_(const std::string &iso);

  int panel_w_{128};
  int panel_h_{64};

  display::Display *display_{nullptr};

  // Background snapshot (RGB565 words, same layout as the LVGL draw buffer).
  // Erase = write the snapshot word back, which restores the exact composited
  // color under the particle (including glyphs).
  std::unique_ptr<uint16_t[]> bg_{};
  bool bg_valid_{false};

  std::vector<Particle> particles_{};
  std::vector<Pixel> pixels_{};
  std::vector<Pixel> prev_pixels_{};
  std::vector<Pixel> sky_pixels_{};
  uint32_t last_render_ms_{0};
  uint32_t last_sky_ms_{0};
  float render_fps_ema_{0.0f};
  float spawn_timer_{0};
  bool precip_active_{false};

  std::string simulate_precip_{""};
  float simulate_precip_strength_{0};
  float simulate_wind_speed_{0};

  int prec_type_{0};
  float prec_strength_{0};
  int wind_speed_{0};

  std::string sun_rising_{};
  std::string sun_setting_{};
  std::string moon_rising_{};
  std::string moon_setting_{};
};

}  // namespace esphome::weather_station
