#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
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

  const std::vector<Pixel> &get_pixels() const { return this->pixels_; }

  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

 protected:
  void update_particles_();
  void update_sky_();
  void get_color_for_precip_(const char *type, uint8_t &r, uint8_t &g, uint8_t &b);
  void angle_to_border_(float angle, int &x, int &y);
  void border_neighbors_(int x, int y, int out[][2], int &count);
  int64_t parse_iso_datetime_(const std::string &iso);

  int panel_w_{128};
  int panel_h_{64};

  std::vector<Particle> particles_{};
  std::vector<Pixel> pixels_{};
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
