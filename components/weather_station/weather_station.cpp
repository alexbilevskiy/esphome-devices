#include "weather_station.h"
#include "esphome/core/log.h"
#include <cstdlib>
#include <cmath>
#include <ctime>

namespace esphome::weather_station {

static const char *const TAG = "weather_station";

static const char *TYPE_RAIN = "rain";
static const char *TYPE_WET_SNOW = "wet_snow";
static const char *TYPE_SNOW = "snow";

static int rand_int(int min_val, int max_val) {
  if (min_val >= max_val)
    return min_val;
  return min_val + (std::rand() % (max_val - min_val + 1));
}

int64_t WeatherStation::parse_iso_datetime_(const std::string &iso) {
  // Expected format: 2024-04-10T08:58:40+03:00
  if (iso.length() < 19)
    return 0;

  struct tm tm{};
  tm.tm_year = std::atoi(iso.substr(0, 4).c_str()) - 1900;
  tm.tm_mon = std::atoi(iso.substr(5, 2).c_str()) - 1;
  tm.tm_mday = std::atoi(iso.substr(8, 2).c_str());
  tm.tm_hour = std::atoi(iso.substr(11, 2).c_str());
  tm.tm_min = std::atoi(iso.substr(14, 2).c_str());
  tm.tm_sec = std::atoi(iso.substr(17, 2).c_str());

  // Parse timezone offset: +03:00 or +0300
  int tz_offset_minutes = 0;
  if (iso.length() >= 25 && (iso[19] == '+' || iso[19] == '-')) {
    int sign = (iso[19] == '-') ? -1 : 1;
    int tz_h = std::atoi(iso.substr(20, 2).c_str());
    int tz_m = 0;
    if (iso.length() >= 25 && iso[22] == ':')
      tz_m = std::atoi(iso.substr(23, 2).c_str());
    else if (iso.length() >= 24)
      tz_m = std::atoi(iso.substr(22, 2).c_str());
    tz_offset_minutes = sign * (tz_h * 60 + tz_m);
  }

  // Manual UTC epoch computation (days since 1970-01-01)
  // Using Howard Hinnant's algorithm — avoids mktime/timegm portability issues
  int y = tm.tm_year + 1900;
  int m = tm.tm_mon + 1;
  int d = tm.tm_mday;
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t) era * 146097 + (int64_t) doe - 719468;

  int64_t epoch = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  epoch -= tz_offset_minutes * 60;
  return epoch;
}

void WeatherStation::angle_to_border_(float angle, int &x, int &y) {
  float cx = (this->panel_w_ - 1) / 2.0f;
  float cy = (this->panel_h_ - 1) / 2.0f;
  float hw = cx;
  float hh = cy;
  float rad = (angle + 180.0f) * M_PI / 180.0f;
  float dx = std::cos(rad);
  float dy = std::sin(rad);

  if (std::abs(dx) < 1e-9f) {
    x = (int) std::round(cx);
    y = (int) std::round(dy > 0 ? cy + hh : cy - hh);
  } else if (std::abs(dy) < 1e-9f) {
    x = (int) std::round(dx > 0 ? cx + hw : cx - hw);
    y = (int) std::round(cy);
  } else {
    float t = std::min(hw / std::abs(dx), hh / std::abs(dy));
    x = (int) std::round(cx + dx * t);
    y = (int) std::round(cy + dy * t);
  }
}

void WeatherStation::border_neighbors_(int x, int y, int out[][2], int &count) {
  count = 0;
  int candidates[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
  for (int i = 0; i < 4; i++) {
    int nx = candidates[i][0];
    int ny = candidates[i][1];
    if (nx < 0 || nx >= this->panel_w_ || ny < 0 || ny >= this->panel_h_)
      continue;
    if (nx == 0 || nx == this->panel_w_ - 1 || ny == 0 || ny == this->panel_h_ - 1) {
      if (count < 2) {
        out[count][0] = nx;
        out[count][1] = ny;
        count++;
      }
    }
  }
}

void WeatherStation::update_sky_() {
  // Sun
  if (!this->sun_rising_.empty() && !this->sun_setting_.empty()) {
    int64_t sr = this->parse_iso_datetime_(this->sun_rising_);
    int64_t ss = this->parse_iso_datetime_(this->sun_setting_);
    int64_t now = (int64_t) time(nullptr);

    if (sr > 0 && ss > 0 && now > 0) {
      int64_t day_len;
      if (sr > ss) {
        day_len = ss - sr + 86400;
      } else {
        day_len = ss - sr;
      }

      float day_angle_span = (float) day_len / 86400.0f * 360.0f;

      int64_t last_sr = (sr < now) ? sr : sr - 86400;
      float sun_angle = (float) (now - last_sr) / 86400.0f * 360.0f;

      // Sunrise/sunset marks
      for (float mark_angle : {0.0f, day_angle_span}) {
        for (float offset : {-0.5f, 0.5f}) {
          int mx, my;
          this->angle_to_border_(mark_angle + offset, mx, my);
          if (mx >= 0 && mx < this->panel_w_ && my >= 0 && my < this->panel_h_) {
            this->pixels_.push_back({(int16_t) mx, (int16_t) my, 200, 60, 0});
          }
        }
      }

      // Sun body
      int sx, sy;
      this->angle_to_border_(sun_angle, sx, sy);
      if (sx >= 0 && sx < this->panel_w_ && sy >= 0 && sy < this->panel_h_) {
        this->pixels_.push_back({(int16_t) sx, (int16_t) sy, 255, 220, 0});
      }
      int neighbors[2][2];
      int ncount;
      this->border_neighbors_(sx, sy, neighbors, ncount);
      for (int i = 0; i < ncount; i++) {
        this->pixels_.push_back({(int16_t) neighbors[i][0], (int16_t) neighbors[i][1], 255, 220, 0});
      }
    }
  }

  // Moon
  if (!this->moon_rising_.empty() && !this->moon_setting_.empty()) {
    int64_t mr = this->parse_iso_datetime_(this->moon_rising_);
    int64_t ms = this->parse_iso_datetime_(this->moon_setting_);
    int64_t now = (int64_t) time(nullptr);

    if (mr > 0 && ms > 0 && now > 0) {
      int64_t up_len;
      if (mr > ms) {
        up_len = ms - mr + 86400;
      } else {
        up_len = ms - mr;
      }

      float up_angle_span = (float) up_len / 86400.0f * 360.0f;

      int64_t last_mr = (mr < now) ? mr : mr - 86400;
      float moon_angle = (float) (now - last_mr) / 86400.0f * 360.0f;

      // Moonrise/moonset marks
      for (float mark_angle : {0.0f, up_angle_span}) {
        for (float offset : {-0.5f, 0.5f}) {
          int mx, my;
          this->angle_to_border_(mark_angle + offset, mx, my);
          if (mx >= 0 && mx < this->panel_w_ && my >= 0 && my < this->panel_h_) {
            this->pixels_.push_back({(int16_t) mx, (int16_t) my, 130, 130, 160});
          }
        }
      }

      // Moon body
      int mx, my;
      this->angle_to_border_(moon_angle, mx, my);
      if (mx >= 0 && mx < this->panel_w_ && my >= 0 && my < this->panel_h_) {
        this->pixels_.push_back({(int16_t) mx, (int16_t) my, 180, 200, 255});
      }
      int neighbors[2][2];
      int ncount;
      this->border_neighbors_(mx, my, neighbors, ncount);
      for (int i = 0; i < ncount; i++) {
        this->pixels_.push_back({(int16_t) neighbors[i][0], (int16_t) neighbors[i][1], 180, 200, 255});
      }
    }
  }
}

void WeatherStation::setup() {
  ESP_LOGCONFIG(TAG, "WeatherStation setup: %dx%d", this->panel_w_, this->panel_h_);
  this->spawn_timer_ = millis();
  this->particles_.reserve(64);
  this->pixels_.reserve(256);
}

void WeatherStation::get_color_for_precip_(const char *type, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (type == TYPE_RAIN) {
    r = 0;
    g = rand_int(100, 150);
    b = rand_int(200, 255);
  } else if (type == TYPE_WET_SNOW) {
    r = rand_int(100, 150);
    g = rand_int(100, 150);
    b = rand_int(100, 150);
  } else if (type == TYPE_SNOW) {
    uint8_t c = rand_int(50, 255);
    r = c;
    g = c;
    b = c;
  } else {
    r = 0;
    g = 0;
    b = 0;
  }
}

void WeatherStation::update_particles_() {
  int prec_type = this->prec_type_;
  float prec_strength = this->prec_strength_;
  int wind_speed = this->wind_speed_;

  if (!this->simulate_precip_.empty()) {
    if (this->simulate_precip_ == "rain") {
      prec_type = 1;
    } else if (this->simulate_precip_ == "wet_snow") {
      prec_type = 2;
    } else if (this->simulate_precip_ == "snow") {
      prec_type = 3;
    }
    prec_strength = this->simulate_precip_strength_;
    if (this->simulate_wind_speed_ > 0) {
      wind_speed = (int) this->simulate_wind_speed_;
    }
  }

  if (prec_type == 0 || prec_strength <= 0) {
    this->particles_.clear();
    this->precip_active_ = false;
    return;
  }

  if (!this->precip_active_) {
    this->spawn_timer_ = (float) millis();
    this->precip_active_ = true;
  }

  int max_drops = (int) (this->panel_h_ * prec_strength * 0.5f);
  if (max_drops < 1)
    max_drops = 1;

  const float speed_rain = 100.0f;
  const float speed_snow = 15.0f;
  const float speed_wet_snow = 25.0f;

  float spawn_speed;
  if (prec_type == 1) {
    spawn_speed = speed_rain;
  } else if (prec_type == 2) {
    spawn_speed = speed_wet_snow;
  } else {
    spawn_speed = speed_snow;
  }
  float interval_ms = (this->panel_h_ / (max_drops * spawn_speed)) * 1000.0f;

  int horizontal_step;
  int horizontal_every;
  if (wind_speed == 0) {
    horizontal_step = 0;
    horizontal_every = 1;
  } else if (wind_speed <= 10) {
    horizontal_step = 1;
    horizontal_every = std::max(1, (int) (10.0f / wind_speed));
  } else {
    horizontal_step = wind_speed / 10;
    horizontal_every = 1;
  }

  uint32_t now = millis();

  float elapsed = (float) (now - this->spawn_timer_);
  int to_spawn = (int) (elapsed / interval_ms);
  if (to_spawn > max_drops)
    to_spawn = max_drops;

  static uint32_t last_dbg = 0;
  if (now - last_dbg > 1000) {
    ESP_LOGI(TAG, "now=%lu spawn_timer=%.1f elapsed=%.1f interval_ms=%.1f to_spawn=%d particles=%d max_drops=%d",
             now, this->spawn_timer_, elapsed, interval_ms, to_spawn, (int) this->particles_.size(), max_drops);
    last_dbg = now;
  }

  for (int s = 0; s < to_spawn && (int) this->particles_.size() < max_drops; s++) {
    const char *drop_type;
    float speed;

    if (prec_type == 1) {
      drop_type = TYPE_RAIN;
      speed = speed_rain;
    } else if (prec_type == 2) {
      if (rand_int(0, 1) == 1) {
        drop_type = TYPE_RAIN;
        speed = speed_rain;
      } else {
        drop_type = TYPE_WET_SNOW;
        speed = speed_wet_snow;
      }
    } else {
      drop_type = TYPE_SNOW;
      speed = speed_snow;
    }

    Particle p;
    p.x = (float) rand_int(0, this->panel_w_ - 1);
    p.y = -1.0f;
    p.timer = now;
    p.type = drop_type;
    p.delay = 1.0f / speed;
    p.h_accum = 0.0f;
    this->get_color_for_precip_(drop_type, p.r, p.g, p.b);
    this->particles_.push_back(p);
  }

  this->spawn_timer_ += (float) to_spawn * interval_ms;

  for (int i = (int) this->particles_.size() - 1; i >= 0; i--) {
    auto &f = this->particles_[i];

    int ix = (int) f.x;
    int iy = (int) f.y;
    if (ix >= 0 && ix < this->panel_w_ && iy >= 0 && iy < this->panel_h_) {
      this->pixels_.push_back({(int16_t) ix, (int16_t) iy, f.r, f.g, f.b});
    }

    uint32_t delta = now - f.timer;
    float drop_delay_ms = f.delay * 1000.0f;
    if (delta < drop_delay_ms)
      continue;

    int distance = (int) std::round((float) delta / drop_delay_ms);
    f.timer += (uint32_t) (distance * drop_delay_ms);

    this->get_color_for_precip_(f.type, f.r, f.g, f.b);

    if (f.type == TYPE_RAIN) {
      f.y += distance;
      if (horizontal_step > 0) {
        f.h_accum += (float) (distance * horizontal_step) / (float) horizontal_every;
        int dx = (int) f.h_accum;
        f.x += dx;
        f.h_accum -= dx;
      }
    } else {
      f.y += distance;
      f.x += rand_int(-1, 1);
    }

    if (f.y > this->panel_h_ - 1) {
      this->particles_.erase(this->particles_.begin() + i);
      continue;
    }

    if (f.x < 0)
      f.x += this->panel_w_;
    if (f.x >= this->panel_w_)
      f.x -= this->panel_w_;
  }
}

void WeatherStation::loop() {
  this->pixels_.clear();
  this->update_sky_();
  this->update_particles_();
}

}  // namespace esphome::weather_station
