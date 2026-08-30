# Weather Station (ESPHome)

LED panel clock and weather station running on ESP32-S3 with ESPHome, LVGL, and a 128x64 HUB75 LED panel. Data source is Home Assistant via the ESPHome native API.

Based on an earlier [Python/Raspberry Pi implementation](https://github.com/alexbilevskiy/weather-station), rebuilt from scratch for ESPHome with LVGL for UI rendering and a custom C++ component for particle/sky-arc graphics.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 with octal PSRAM (80MHz) |
| Display | 128x64 HUB75 LED panel, 1/32 scan (5 address lines) |
| Framework | ESP-IDF |
| Onboard RGB LED | WS2812 (GPIO48) |
| TX indicator LED | GPIO43 (inverted) |
| Status LED | GPIO44 (inverted) |

### HUB75 pin mapping

| Signal | GPIO | Signal | GPIO |
|--------|------|--------|------|
| R1 | GPIO4 | R2 | GPIO6 |
| G1 | GPIO1 | G2 | GPIO2 |
| B1 | GPIO5 | B2 | GPIO7 |
| A | GPIO15 | B | GPIO41 |
| C | GPIO16 | D | GPIO40 |
| E | GPIO42 | LAT | GPIO39 |
| OE | GPIO18 | CLK | GPIO17 |

## Software Architecture

Two layers work together:

1. **LVGL widgets** (YAML-defined) — labels and images for all text and icon rendering. Positions are hardcoded, no runtime layout engine.
2. **C++ `weather_station` component** — holds stateful logic for precipitation particles and sun/moon arc positions. Produces a vector of pixels that a YAML interval lambda draws onto a transparent LVGL canvas overlay.

This split keeps the component LVGL-agnostic (no UI calls in C++) while allowing per-pixel drawing that LVGL widgets can't do natively.

### Rendering pipeline

- LVGL refreshes the display at 16ms intervals (~60 FPS) via the HUB75 display component. Actual refresh rate is measured via `on_draw_end` trigger (`LV_EVENT_REFR_READY`).
- A 20ms interval (50 FPS) clears and redraws the particle canvas from `get_pixels()`. Actual execution rate is measured via EMA of inter-interval delta.
- Labels are refreshed on two intervals: 1s for time-sensitive widgets (clock, date, temp outside blinking, FPS display), 5s for the rest.
- A 30s interval updates brightness based on time-of-day and sun position.

### Display configuration

The HUB75 display is configured with `update_interval: never` and `auto_clear_enabled: false` — LVGL manages all screen updates. `buffer_size: 100%` ensures the LVGL draw buffer holds a complete frame (needed by the `lvgl_screenshot` component). The 100% buffer in octal PSRAM is sufficient.

## File Structure

```
/opt/src/esphome/
├── weather-station.yaml           # main ESPHome config (841 lines)
├── weather-station.md             # this document
├── fonts/
│   ├── win_crox5h.bdf             # clock font (18px, proportional)
│   ├── helvR08.bdf                # regular font (8px, proportional)
│   ├── b10.bdf                    # small font (10px, monospace)
│   └── (other unused fonts)
├── icons/
│   └── (27 PNG icons, 8x8, Yandex weather icon set)
└── components/
    ├── weather_station/
    │   ├── __init__.py            # config schema + codegen
    │   ├── weather_station.h      # class declaration (Particle, Pixel structs)
    │   └── weather_station.cpp    # particle system + sky arc implementation
    └── lvgl_screenshot/
        └── (screenshot/debug component, serves canvas over HTTP on port 8080)
```

## Display Layout (128x64)

```
 0 ┌─────────────────────────────────────────────────────────┐
   │ HH:MM                                    21.3°  [icon]°  │
   │                                          (inside) (outside)
   │                                                          │
   │ Sat 24 Aug                  [icon]e18°  [icon]m15°       │
   │                             (forecast1) (forecast2)      │
20 ├─────────────────────────────────────────────────────────┤
   │ 45%                       18°  [icon]d20°                │
   │ (humidity)                (forecast2)                    │
   │                                                          │
   │ 620ppm                            ne 5m/s                │
40 ├─────────────────────────────────────────────────────────┤
   │ (CO2)                           (wind)                   │
   │                                                          │
   │                                                          │
49 ├─────────────────────────────────────────────────────────┤
   │ custom text line 1 (font_small, monospace)               │
   │ custom text line 2                                       │
63 └─────────────────────────────────────────────────────────┘
```

Sun and moon indicators travel along the screen perimeter — upper half when above horizon, lower half when below. Two dim horizon pixels mark the left and right edges of the horizon line (y = 32).

### Widget positions

| Widget | Font | x | y | Align | Content |
|--------|------|---|---|-------|---------|
| clock_label | font_clock (18px) | 1 | -4 | TOP_LEFT | `HH:MM` |
| date_label | font_reg (8px) | 1 | 20 | TOP_LEFT | `Sat 24 Aug` |
| temp_inside_label | font_reg | -1 | 0 | TOP_RIGHT | `21.3°` |
| temp_outside_group | — | -1 | 10 | TOP_RIGHT | flex: [icon, label] |
| forecast_group_1 | — | -1 | 20 | TOP_RIGHT | flex: [icon, label] |
| forecast_group_2 | — | -1 | 30 | TOP_RIGHT | flex: [icon, label] |
| humidity_label | font_reg | 1 | 30 | TOP_LEFT | `45%` |
| co2_label | font_reg | 1 | 40 | TOP_LEFT | `620ppm` |
| wind_label | font_reg | -1 | 40 | TOP_RIGHT | `ne 5m/s` |
| custom_text_line1 | font_small (10px) | 1 | 49 | TOP_LEFT | word-wrapped text |
| custom_text_line2 | font_small | 1 | 54 | TOP_LEFT | word-wrapped text |
| fps_label | font_small (10px) | 1 | 49 | TOP_LEFT | `L:60 I:50` (replaces custom text when FPS mode on) |
| particle_canvas | — | 0 | 0 | — | 128x64 transparent overlay |

Right-aligned icon+label pairs use LVGL flex containers (`width: SIZE_CONTENT`, `flex_flow: row`, `pad_column: 1px`) that auto-pack the icon to the left of the label with a 1px gap. When label text changes width, the container auto-resizes and stays anchored to the right edge.

### Fonts

| Font ID | File | Size | Type | Glyph width | Usage |
|---------|------|------|------|-------------|-------|
| font_clock | win_crox5h.bdf | 18px | Proportional | digit=13, colon=6 | Clock display |
| font_reg | helvR08.bdf | 8px | Proportional | digit=6, colon=3, `1`=4 | All weather labels |
| font_small | b10.bdf | 10px | Monospace | 5px (all chars) | Custom text |

The clock font (`win_crox5h.bdf`) required a charset fix: the original `CHARSET_REGISTRY "RAWIN"` / `CHARSET_ENCODING "R"` was unrecognized by FreeType. Changed to `ISO10646` / `1` to match the other BDF fonts. Without this fix, FreeType returned `char_index=0` for all codepoints, causing an empty glyph set and a crash in `Font::find_glyph()`.

The small font (`b10.bdf`) includes Latin, Greek, and Cyrillic glyphsets for international text support in the custom text feature.

## Features

### Clock and Date

LVGL label widgets with built-in time formatting. Synchronized from Home Assistant time (`platform: homeassistant`). Clock shows `HH:MM`, date shows abbreviated weekday, day, and month (`Sat 24 Aug`).

Refreshed every 1 second.

### Temperature — Inside

Displays indoor temperature from an HA sensor, formatted as `21.3°` (one decimal place). Right-aligned at top.

### Temperature — Outside (blinking)

Alternates between two data sources every 5 seconds:
- **Seconds 5-9 of each 10-second cycle**: measured temperature from outdoor sensor (bright cyan `#146E6E`)
- **Seconds 0-4**: forecast-provided temperature from Yandex Weather (dim cyan `#0A3C3C`)

Shows a weather icon (current conditions) to the left of the temperature, packed in a flex container.

Refreshed every 1 second (needs second-precision for the blink timing).

### CO2

Indoor CO2 level in ppm, formatted as `620ppm`. Left-aligned.

### Humidity

Indoor humidity percentage, formatted as `45%`. Left-aligned.

### Wind

Wind direction and speed, formatted as `ne 5m/s`. Direction is derived from bearing degrees via a switch statement (0=calm, 45=ne, 90=e, 135=se, 180=s, 225=sw, 270=w, 315=nw, 360=n). Unknown bearings show `?`. Speed comes from Yandex Weather's `wind_speed` attribute. Right-aligned.

### Forecast

Two forecast rows, each showing a period letter, temperature, and weather icon:
- Period: `m` (morning, 6-12h), `d` (day, 12-18h), `e` (evening, 18h+), `n` (night, 0-6h)
- Format: `e18°` with icon to the left

Forecast data is flattened into individual HA template sensors (`forecast_temp_1/2`, `forecast_icon_1/2`, `forecast_period_1/2`) because HA's ESPHome integration serializes array attributes as Python `repr()` strings (not valid JSON), making on-device parsing impractical.

### Custom Text

User-defined text displayed at the bottom of the panel in the monospace small font (5px/char, 25 chars/line). If text exceeds 25 characters, it splits at the last space before the cutoff and renders on two lines. UTF-8 aware character counting. If no space is found, hard-cuts at 25 characters.

Input via a template text entity (`Custom text`) exposed to Home Assistant. Text is cleared (not hidden) when empty or in extra-dim mode to avoid redraw artifacts.

### FPS Monitor

Real-time FPS display showing two metrics, toggled via a `Show FPS` switch entity exposed to Home Assistant:

| Metric | Label | Source | Measurement |
|--------|-------|--------|-------------|
| LVGL refresh rate | `L` | `on_draw_end` trigger (`LV_EVENT_REFR_READY`) | Counts complete refresh cycles per second |
| Interval execution rate | `I` | 20ms canvas redraw interval | EMA of `1000/delta_ms` between interval firings |

When FPS mode is on, the custom text widgets are hidden and replaced by the FPS label at the same position (y=49). FPS is also logged at INFO level every second. FPS label remains visible in extra-dim mode.

The LVGL FPS reflects the actual display refresh rate (driven by the 16ms LVGL refresh timer and ESPHome loop speed). The interval FPS shows the real execution rate of the 20ms particle canvas redraw, revealing delays from WiFi, API traffic, or particle system load.

### Precipitation Particles

Animated particle system rendered on a transparent full-screen canvas overlay. Supports three precipitation types:

| Type | Code | Speed | Color |
|------|------|-------|-------|
| Rain | 1 | 100 px/s | Blue range (g: 100-150, b: 200-255) |
| Wet snow | 2 | Mixed (rain=100, snow=25 px/s) | Gray range (100-150 per channel) |
| Snow | 3 | 15 px/s | White range (50-255, equal RGB) |

Wet snow spawns a random mix of rain and wet snow particles per drop.

**Particle physics:**
- Spawn rate: `max_drops = panel_height * strength * 0.5`, spawn interval = `panel_height / (max_drops * spawn_speed)`
- Wind drift: horizontal step based on wind speed (0-10 m/s: 1px every N frames; 10+: N px every frame)
- Each particle has an independent timer that advances by `distance * delay_ms` (not snapped to current time) to prevent lockstep/waves from variable loop frequency
- Particles wrap horizontally around the panel
- Particles are removed when they pass the bottom edge

**Data sources:**
- Real weather: `sensor.precipitation_type`, `sensor.precipitation_strength`, `sensor.wind_speed` from HA
- Simulation overrides: template select/number entities allow testing without real weather data

The `precip_active_` flag resets the spawn timer when precipitation starts, preventing a stale timer from spawning all particles at once.

Canvas updates at 20ms (50 FPS). The component's `loop()` updates particle state continuously; the interval lambda reads `get_pixels()` and draws via `lv_canvas_set_px()`.

### Sun/Moon Arc

Sun and moon position indicators travel along the **screen perimeter** — the projection of their circular sky path onto the rectangle edges. The middle of the screen (y = panel_height/2) represents the horizon.

**Above horizon (day for sun, night for moon):**
- The body travels along the **upper half perimeter**, **left to right**
- Path: `(0, horizon) → (0, 0) → (panel_width-1, 0) → (panel_width-1, horizon)`
- Progress (0.0–1.0) = `(now - rise) / (set - rise)`, mapped linearly along the perimeter path

**Below horizon (night for sun, day for moon):**
- The body travels along the **lower half perimeter**, **right to left** (mirrored, visually continuing the arc)
- Path: `(panel_width-1, horizon) → (panel_width-1, panel_height-1) → (0, panel_height-1) → (0, horizon)`
- Progress = `(now - set) / (86400 - day_length)`, mapped linearly along the perimeter path

Transitions are smooth: sunrise/moonrise at `(0, horizon)`, sunset/moonset at `(panel_width-1, horizon)`.

**Horizon indicators:** Two dim gray (40, 40, 40) pixels at the left and right edges of the horizon line (y = panel_height/2), always visible.

**Body rendering:** Each body (sun/moon) is drawn as a main pixel plus up to 2 adjacent neighbors for a thicker indicator:
- Sun: yellow (255, 220, 0)
- Moon: light blue (180, 200, 255)

**Time handling:** ISO 8601 datetime strings from HA are parsed to UTC epoch seconds using a manual implementation (Howard Hinnant's days-from-civil algorithm). `time(nullptr)` also returns UTC epoch, so the difference is timezone-correct. No `mktime`/`timegm` used (portability issues on ESP-IDF).

The previous border-perimeter arc model (analog-clock style around the full panel border) is preserved in `update_sky_border_()` but not called.

### Auto-Brightness

Brightness adjusts automatically based on time of day and sun position:

| Time | Sun position | Brightness (0-100) | Extra dim |
|------|-------------|-------------------|-----------|
| 0:00-6:00 | Below horizon | 1 | Yes |
| 0:00-6:00 | Above horizon | 20 | No |
| 6:00-9:00 | Below horizon | 20 | No |
| 6:00-9:00 | Above horizon | 50 | No |
| 9:00-18:00 | — | 60 | No |
| 18:00-22:00 | — | 25 | No |
| 22:00-24:00 | — | 3 | No |

**Extra dim mode** (brightness = 1): hides all non-essential widgets (date, temperatures, CO2, humidity, wind, forecast, custom text). Only the clock and particle canvas (sky arc + precipitation) remain visible. Clock color switches from white (255,255,255) to dim gray (40,40,40) to compensate for the HUB75 driver's brightness curve difference vs BCM-based drivers.

**Manual override:** A `Brightness` light entity (monochromatic) allows the user to set a fixed brightness. When ON, auto-brightness is bypassed. When OFF, auto-brightness resumes. Setting brightness to 1 via the slider also triggers extra dim mode.

Brightness conversion: HUB75 uses 0-255 scale. `brightness_255 = round(brightness_100 * 2.55)`. Special case: `brightness == 1` maps to `1/255` (not `3/255` from rounding) to avoid excessive brightness at the lowest setting.

Updated on two triggers: light state change (immediate) and 30s interval (automatic transitions).

### Weather Icons

27 Yandex weather icons (8x8 PNG) compiled into ESPHome at build time via `image: platform: file, type: RGB`. Dynamic icon selection uses the `mapping:` component to map HA text sensor values (e.g. `bkn_d`, `ovc_ra`, `skc_n`) to image IDs at runtime.

Image ID naming: `+` → `p`, `-` → `m` (e.g. `bkn_+ra_d` → `icon_bkn_pra_d`) for valid C++ identifiers.

Icons are rendered at full brightness (the original Python implementation dimmed icons to 70%; this was an artifact of the rendering pipeline and is intentionally not replicated).

## Home Assistant Integration

### Data sources

All data flows via ESPHome native API (no MQTT, no HTTP polling). HA entities are imported using `platform: homeassistant` sensors and text sensors. State updates are push-based (real-time).

| ESPHome ID | HA Entity | Type | Feeds |
|------------|-----------|------|-------|
| temp_inside | sensor.aqara_weather_02_temperature | sensor | temp_inside_label |
| temp_outside_measured | sensor.tuya_weather_02_temperature | sensor | temp_outside_label (measured) |
| temp_outside_provided | weather.yandex_weather (attr: temperature) | text_sensor | temp_outside_label (provided) |
| co2 | sensor.d1_co2_co2_scd30 | sensor | co2_label |
| humidity | sensor.aqara_weather_02_humidity | sensor | humidity_label |
| current_icon | sensor.fact_icon | text_sensor | weather_icon |
| wind_bearing | weather.yandex_weather (attr: wind_bearing) | text_sensor | wind_label |
| wind_speed_weather | weather.yandex_weather (attr: wind_speed) | text_sensor | wind_label |
| sun_state | sun.sun | text_sensor | brightness logic |
| sun_rising | sun.sun (attr: next_rising) | text_sensor | sky arc (sun) |
| sun_setting | sun.sun (attr: next_setting) | text_sensor | sky arc (sun) |
| moon_rising | sensor.home_moon_rise | text_sensor | sky arc (moon) |
| moon_setting | sensor.home_moon_set | text_sensor | sky arc (moon) |
| precip_type | sensor.precipitation_type | sensor | particle system |
| precip_strength | sensor.precipitation_strength | sensor | particle system |
| wind_speed_real | sensor.wind_speed | sensor | particle system (wind drift) |
| forecast_temp_1 | sensor.forecast_temp_1 (HA template) | sensor | forecast_label_1 |
| forecast_icon_1_state | sensor.forecast_icon_1 (HA template) | text_sensor | forecast_icon_1 |
| forecast_period_1 | sensor.forecast_period_1 (HA template) | text_sensor | forecast_label_1 |
| forecast_temp_2 | sensor.forecast_temp_2 (HA template) | sensor | forecast_label_2 |
| forecast_icon_2_state | sensor.forecast_icon_2 (HA template) | text_sensor | forecast_icon_2 |
| forecast_period_2 | sensor.forecast_period_2 (HA template) | text_sensor | forecast_label_2 |

### HA-side prerequisites

Six template sensors must exist in Home Assistant to flatten the Yandex Weather forecast array into individual entities:
- `sensor.forecast_temp_1` / `sensor.forecast_temp_2` — from `forecast[1/2].native_temperature`
- `sensor.forecast_icon_1` / `sensor.forecast_icon_2` — from `forecast_icons[0/1]`
- `sensor.forecast_period_1` / `sensor.forecast_period_2` — time-of-day letter (`m`/`d`/`e`/`n`) derived from forecast datetime hour

All template sensors should have availability templates guarding against missing or short forecast data.

### Controllable entities (exposed to HA)

| Entity | Type | Purpose |
|--------|------|---------|
| Brightness | light (monochromatic) | On/off + brightness slider. OFF = auto mode. |
| Custom text | text | User text input (up to 150 chars) |
| Show FPS | switch | Toggle FPS overlay (replaces custom text) |
| Simulate precipitation | select | Options: "", "snow", "rain", "wet_snow" |
| Simulated precip strength | number (slider) | 0.0-2.0, step 0.5 |
| Simulated wind speed | number (slider) | 0.0-30.0, step 1.0 |
| RGB Led | light (esp32_rmt_led_strip) | Onboard WS2812 LED |
| Indicator TX | light (binary) | TX indicator LED |
| Restart | button | Diagnostic restart |
| WiFi Signal Sensor | sensor | WiFi signal strength |
| Uptime | sensor | Device uptime |

## C++ Component: `weather_station`

### Config schema

```python
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(WeatherStation),
    cv.Required("panel_width"): cv.int_,
    cv.Required("panel_height"): cv.int_,
})
```

### Data structures

```cpp
struct Particle {
    float x, y;
    uint32_t timer;
    uint8_t r, g, b;
    const char *type;    // "rain", "wet_snow", "snow"
    float delay;         // seconds per pixel
    float h_accum;       // horizontal drift accumulator
};

struct Pixel {
    int16_t x, y;
    uint8_t r, g, b;
};
```

### Public API

```cpp
// Pixel vector for YAML lambda to draw
const std::vector<Pixel> &get_pixels() const;

// Simulation setters (from template entities)
void set_simulate_precip(const std::string &v);
void set_simulate_precip_strength(float v);
void set_simulate_wind_speed(float v);

// Real weather data setters (from HA sensors)
void set_precip_type(int v);
void set_precip_strength(float v);
void set_wind_speed_real(int v);

// Sky data setters (from HA text sensors)
void set_sun_rising(const std::string &v);
void set_sun_setting(const std::string &v);
void set_moon_rising(const std::string &v);
void set_moon_setting(const std::string &v);
```

### loop() method

Called by ESPHome's main loop (every few ms). Clears the pixel vector, then:
1. `update_sky_()` — calculates sun/moon positions using sine-curve sky model and appends pixels to the vector (including horizon indicators)
2. `update_particles_()` — updates particle state (spawn, movement, removal) and appends pixels to the vector

The YAML interval lambda (20ms) reads `get_pixels()` and draws each pixel via `lv_canvas_set_px()`. This keeps the component LVGL-agnostic.

### Key implementation details

- **Spawn timing**: Uses a float `spawn_timer_` that advances by `to_spawn * interval_ms` regardless of how many particles actually spawned. Prevents debt accumulation when particles are at max capacity.
- **Independent particle timers**: Each particle's timer advances by `distance * delay_ms` rather than snapping to `now`. Prevents lockstep/wave patterns caused by variable `loop()` call frequency.
- **ISO datetime parsing**: Manual implementation using Howard Hinnant's days-from-civil algorithm. Parses timezone offsets (`+03:00` or `+0300`). Avoids `mktime`/`timegm` portability issues on ESP-IDF. `time(nullptr)` returns UTC epoch (timezone setting only affects `localtime()`/`strftime()`, not `time()`).
- **Sky position model**: `sky_position_()` converts rise/set times and current time to (x, y) coordinates on the screen perimeter. The upper half perimeter (left edge up → top edge → right edge down) is used when above horizon; the lower half perimeter (right edge down → bottom edge → left edge up) is used when below, traversed right-to-left. Rise time is normalized to the most recent occurrence to handle HA's "next" events correctly. `sky_neighbors_()` finds up to 2 adjacent pixels for thicker body rendering.
- **Previous arc model**: `update_sky_border_()` preserves the old border-perimeter analog-clock arc with `angle_to_border_()` and `border_neighbors_()`. Not called but retained for reference.

## Colors

| Color ID | RGB | Used by |
|----------|-----|---------|
| color_clock | 255, 255, 255 | Clock label |
| color_clock_dim | 40, 40, 40 | Clock label in extra dim mode |
| color_date | 80, 80, 80 | Date, custom text |
| color_temp_inside | 2, 100, 12 | Inside temperature |
| color_temp_outside | 20, 110, 110 | Outside temp (measured) |
| color_temp_outside_provided | 10, 60, 60 | Outside temp (provided) |
| color_co2 | 80, 80, 80 | CO2 |
| color_humidity | 80, 80, 80 | Humidity |
| color_wind | 20, 60, 110 | Wind |
| color_forecast | 60, 20, 60 | Forecast |

Colors use raw 0-255 integer values (`red_int`/`green_int`/`blue_int`) to match the original Python config exactly, with no percentage rounding.

## Not Yet Implemented

- **RGB light mode** — fill entire display with a solid color (would use `lv_canvas_fill_bg` on the overlay canvas, hiding all labels)
- **Debug borders** — toggle rectangles around widget bounds on the canvas (would use `lv_canvas_draw_rectangle`)

## Debugging

The `lvgl_screenshot` component serves the LVGL canvas over HTTP on port 8080 as a PNG image, useful for remote debugging without physical access to the panel.
