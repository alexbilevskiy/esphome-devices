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
| ALS sensor | None onboard — ambient lux received via UDP packet_transport from the `presence-bedroom` device (BH1750) |

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

1. **LVGL widgets** (YAML-defined) — labels and images for all text and icon rendering. Positions are hardcoded, no runtime layout engine. LVGL re-renders only when widgets refresh (~2 full renders/s: 1s clock tick, 5s data ticks).
2. **C++ `weather_station` component** — holds stateful logic for precipitation particles and sun/moon arc positions. It draws overlay pixels **directly into the HUB75 framebuffer** (bypassing LVGL), paced at a 20ms tick inside its own `loop()`.

This split keeps the component LVGL-agnostic in the sense that it only talks to `display::Display` (never to the LVGL API). The overlay drawing path does not go through LVGL at all — LVGL never sees the particles, so they never trigger invalidations.

### Rendering pipeline

- LVGL renders the static UI when widgets are invalidated (~2 times/s). The ESPHome main loop interval is set to 1ms (`App.set_loop_interval(1)` on boot) to minimize sleep overhead.
- The `weather_station` component paces its own 20ms render tick inside `loop()`: erase the previous overlay frame, advance particle state, draw the new frame. All writes go to the HUB75 framebuffer via `draw_pixels_at()` (1x1 RGB565 words).
- **Background snapshot:** on every real LVGL render (`on_render_ready` → `LV_EVENT_RENDER_READY`, fired only when something was actually rendered), the YAML lambda passes the LVGL draw buffer (`lv_display_get_buf_active()`) to `ws.on_frame_composited()`. The component copies it row-by-row (stride-aware, RGB565) into a background snapshot and redraws the current overlay pixels on top. `full_refresh: true` (single full-size buffer, FULL render mode) makes the buffer a persistent full-screen composited frame: only dirty areas are re-rendered into it, but the buffer always holds the complete frame — exactly what the HUB75 framebuffer holds before overlay drawing. A size guard rejects any snapshot whose dimensions don't match the panel (would be a partial-area render — copying it would corrupt the snapshot).
- **Erase:** each particle's previous position is restored by writing the background snapshot word back — byte-for-byte the same RGB565 word, through the same `draw_pixels_at()` conversion path, so it reproduces the exact pixel that LVGL composited there (including glyphs under the particle). This is what makes the overlay artifact-free.
- Before the first snapshot arrives (`bg_valid_ == false`), only drawing happens (no erase) — the HUB75 buffer is zeroed (black) at boot, so this is visually identical.
- Labels are refreshed on two intervals: 1s for time-sensitive widgets (clock, date, temp outside blinking, FPS display), 5s for the rest.
- A 30s interval updates brightness based on time-of-day and sun position.

### Display configuration

The HUB75 display is configured with `update_interval: never` and `auto_clear_enabled: false` — LVGL manages all screen updates. `buffer_size: 100%` + `full_refresh: true` (single full-size buffer, FULL render mode) is required by the overlay architecture: the buffer is a persistent full-screen composited frame that `weather_station` copies as its background snapshot (and `lvgl_screenshot` reads for debugging). Note: PARTIAL mode (without `full_refresh`) reshapes the buffer to each dirty area (`buf_area = inv_area`, stride per-area in lv_refr.c), so the buffer is NOT an accumulated frame there — this was the cause of a corrupted-snapshot bug (screen-wide garbage, particle erosion, "jumping" widgets) that `full_refresh: true` fixed.

## File Structure

```
/opt/src/esphome/
├── weather-station.yaml           # main ESPHome config (1191 lines)
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

Particle/sky overlay pixels are drawn by the `weather_station` component directly into the HUB75 framebuffer (no LVGL widget involved).

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

Wind direction and speed, formatted as `ne 5m/s`. Direction is derived from bearing degrees via a switch statement (0=calm, 45=ne, 90=e, 135=se, 180=s, 225=sw, 270=w, 315=nw, 360=n). Unknown bearings show `?`. Speed comes from `sensor.wind_speed` — the same source the particle wind drift uses, so the label and the rain always agree. Right-aligned.

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
| LVGL render rate | `L` | `on_render_ready` trigger (`LV_EVENT_RENDER_READY`) | Counts real render cycles per second (~2 when idle: 1s clock + 5s data ticks). Note: `on_draw_end` (`LV_EVENT_REFR_READY`) fires every 16ms refr-timer cycle even with nothing rendered, so it is NOT used for this metric. |
| Overlay render rate | `I` | `weather_station` 20ms render tick | EMA of `1000/delta_ms` between component render ticks (exposed via `get_render_fps()`) |

When FPS mode is on, the custom text widgets are hidden and replaced by the FPS label at the same position (y=49). FPS is also logged at INFO level every second. FPS label remains visible in extra-dim mode.

`L` shows how often LVGL actually re-renders the UI (it should sit at ~2; spikes when widgets change rapidly). `I` shows the real execution rate of the 20ms overlay render tick, revealing delays from WiFi, API traffic, or particle system load.

### Precipitation Particles

Animated particle system drawn by the component directly into the HUB75 framebuffer (overlay layer above LVGL's output). Supports three precipitation types:

| Type | Code | Speed | Color |
|------|------|-------|-------|
| Rain | 1 | 100 px/s | Blue range (g: 100-150, b: 200-255) |
| Wet snow | 2 | Mixed (rain=100, snow=25 px/s) | Gray range (100-150 per channel) |
| Snow | 3 | 15 px/s | White range (50-255, equal RGB) |

Wet snow spawns a random mix of rain and wet snow particles per drop.

**Particle physics:**
- Spawn rate: `max_drops = panel_height * strength * 0.5`, spawn interval = `panel_height / (max_drops * spawn_speed)`
- Wind drift: quadratic — `ratio = (min(wind, 9) / 9)^2` px sideways per px fallen (45° at 9 m/s, clamped above; e.g. 3 m/s → ~7 px over the full panel height). Applied to rain and wet-snow rain drops; snow uses a random ±1 px walk and is unaffected by wind
- Each particle has an independent timer that advances by `distance * delay_ms` (not snapped to current time) to prevent lockstep/waves from variable loop frequency
- Particles wrap horizontally around the panel
- Particles are removed when they pass the bottom edge

**Data sources:**
- Real weather: `sensor.precipitation_type`, `sensor.precipitation_strength`, `sensor.wind_speed` from HA
- Simulation overrides: template select/number entities allow testing without real weather data

The `precip_active_` flag resets the spawn timer when precipitation starts, preventing a stale timer from spawning all particles at once.

Overlay updates at 20ms (50 FPS), paced inside the component's own `loop()` (main loop runs at 1ms). Each tick: erase previous overlay pixels from the background snapshot, advance particle state, draw new pixels via `display->draw_pixels_at()`. Background snapshot is synced from the LVGL draw buffer on every real render (`on_render_ready`).

#### Performance profile and optimization history

**Original canvas approach (replaced).** Measured with `runtime_stats:` (per-component loop times): the canvas-driven render path cost ~35ms per LVGL frame during precipitation (~26-29 effective FPS). Every `lv_canvas_set_px`/`lv_canvas.fill` call invalidates the whole full-screen canvas (`lv_obj_invalidate` inside lv_canvas.c, per LVGL docs "this function invalidates the canvas object every time"), so each 20ms tick forced a full-frame LVGL software render (background + all labels + ARGB8888 canvas alpha blend) plus the HUB75 per-pixel bit-plane scatter. This was the single dominant cost of the whole device (~92% of main-loop active time).

**Current approach: direct-to-framebuffer overlay with LVGL-buffer background snapshot.** The key insight that unlocked it: with `full_refresh: true` (single full-size buffer, FULL render mode) the LVGL draw buffer is a persistent full-screen composited frame — dirty areas are re-rendered into it at screen coordinates, and the whole buffer is flushed each cycle. After a real render, `lv_display_get_buf_active()` holds the complete UI frame *without* overlay pixels (LVGL never saw them). Copying it (16 KiB RGB565) into the component's background snapshot provides the exact composited color under every particle — the piece both earlier attempts were missing. The snapshot is taken in `on_render_ready` (`LV_EVENT_RENDER_READY`, fired only when something was actually rendered — unlike `LV_EVENT_REFR_READY`, which fires every 16ms refr-timer cycle regardless), and the current overlay pixels are redrawn on top (the flush overwrote them).

**First attempt of this approach was corrupted (fixed):** without `full_refresh`, PARTIAL render mode reshapes the draw buffer to each dirty area (`buf_area = inv_area`, area-sized stride, lv_refr.c:868-872), so the buffer held the last rendered *strip*, not an accumulated frame. The snapshot copy was garbage across the whole panel: screen-wide glitch pixels, particle erosion restored wrong colors, "jumping" widgets (ghost pixels over labels). Fixed by switching to `full_refresh: true` and adding a w/h guard in `on_frame_composited()` that invalidates the snapshot on any non-full-screen frame.

Result: LVGL renders ~2 times/s (≈18ms full-frame render+flush each) instead of 50 times/s (≈34ms each); the `lvgl` runtime avg drops from ~34.6ms to fractions of a millisecond. Overlay rendering costs ~150 1x1 `draw_pixels_at` calls per 20ms tick (sub-millisecond). Particle animation runs at a steady 50 FPS. Cost: +16 KiB internal RAM for the snapshot, −32 KiB ARGB8888 canvas buffer, full-frame flush only ~2 times/s.

**Status: verified on hardware (2026-09-18), accepted as the final architecture.** No screen-wide glitches, no particle erosion, no widget ghosting; particles render cleanly over labels and custom text. This closes the FPS investigation — earlier failed attempts and the corrupted first iteration are kept below for history only.

**Earlier failed attempts (kept for history):**

1. **Partial invalidation via direct canvas buffer writes** — wrote ARGB8888 pixels straight into the canvas draw buffer (`lv_canvas_get_draw_buf()`) bypassing the invalidating `lv_canvas_set_px`, then invalidated only dirty row bands via `lv_obj_invalidate_area()`. Result: no FPS gain with precipitation (rain spreads over most of the 64 rows within a second, so the ">50% rows dirty → full-screen fallback" branch fired nearly every tick) plus visual artifacts (stale vertical lines: the prev-pixel snapshot was taken at the component's 1kHz loop rate, not at the 20ms draw tick, so most drawn pixels were never erased). Clean 50 FPS without precipitation confirmed the mechanism itself works. Reverted.
2. **Direct-to-panel drawing without a snapshot** — drew/erased particles straight into the HUB75 framebuffer via `matrix.draw_pixel_at()`, redrawing after `on_draw_end`. Result: stable 50 FPS, but severe artifacts: erasing a moved particle blanks the pixel it crossed over a widget glyph, and widgets don't re-render until their 1s/5s tick — particles "erode" labels pixel-by-pixel. The current approach fixes exactly this by restoring erased pixels from the background snapshot. Superseded.

Also rejected by analysis (not tested): particles as individual LVGL widgets — LVGL's invalid-area buffer is 32 entries (`LV_INV_BUF_SIZE`); the 33rd area resets to a full-screen invalidation, and 40-74 particles × old+new positions overflow it every refresh. Strictly worse than the canvas.

Rejected by research (not tested): RGB565A8 canvas format (ESPHome canvas codegen only supports ARGB8888-transparent / RGB565-opaque, and it would still cost a full blend + scatter per tick); LVGL SW-draw asm optimizations (LVGL 9.5 has no Xtensa/ESP32-S3 asm paths — only NEON/Helium/RISC-V); an esp-hub75 framebuffer readback API (unnecessary — the LVGL buffer is readable via public API); PPA (LVGL 9.5 supports it on ESP32-P4 only). Note: `full_refresh: true` was initially marked as unnecessary based on a wrong assumption that a PARTIAL-mode full-size buffer accumulates the frame — it turned out to be required (see the corrupted-snapshot fix above).

Accepted micro-optimizations (kept): `byte_order: little_endian` on the `lvgl:` block (skips the per-frame RGB565 byte swap; colors verified correct, no measurable FPS gain) and the brightness write action hiding/showing widgets only on extra-dim transitions, plus the auto-brightness feedback call skipping publication when the computed brightness is unchanged.

`bit_depth: 6` on the hub75 display was also tested (slight FPS gain, under 2) but garbles colors on this panel and was reverted.

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

Brightness is controlled via the `Brightness` light entity and the `Auto Brightness Mode` select (template, restored after reboot, default `als`) with three modes. The light entity is fully compliant and always reflects the panel's actual brightness: in `manual` mode it is the driver, in auto modes it displays the computed value.

**Mode `time`** — time of day and sun position:

| Time | Sun position | Brightness (0-100) | Extra dim |
|------|-------------|-------------------|-----------|
| 0:00-6:00 | Below horizon | 1 | Yes |
| 0:00-6:00 | Above horizon | 20 | No |
| 6:00-9:00 | Below horizon | 20 | No |
| 6:00-9:00 | Above horizon | 50 | No |
| 9:00-18:00 | — | 60 | No |
| 18:00-22:00 | — | 25 | No |
| 22:00-24:00 | — | 3 | No |

**Mode `als`** — ambient light level. Illuminance (lux) is received via UDP packet transport from the `presence-bedroom` device (BH1750 read at 1s, broadcast every 1s as `bh1750_lux`, consumed as internal `ambient_lux`; the HA-visible entity on the provider publishes on >= 1 lux change or a 60s heartbeat so HA is not flooded). Lux is mapped to brightness (1-100) via a step clamp (no interpolation, no smoothing): 5 thresholds define 6 bands, each band has its own brightness level. Note: the first threshold must be > 0, since `lux < 0` is unreachable (lux >= 0) and would leave band 1 dead:

| Band | Lux range (defaults) | Default brightness |
|------|----------------------|--------------------|
| 1 | < 1 | 1 |
| 2 | 1-24.99 | 3 |
| 3 | 25-39.99 | 25 |
| 4 | 40-99.99 | 40 |
| 5 | 100-199.99 | 50 |
| 6 | >= 200 | 70 |

All 11 values (5 lux thresholds + 6 brightness levels) are editable from Home Assistant via template number entities (`Curve Lux 1-5`, `Curve Brightness 1-6`) and are restored after reboot (`restore_value: true`). Lux thresholds are clamped to remain strictly increasing. No hysteresis — a lux value oscillating at a band boundary will toggle brightness between the adjacent bands.

**Extra dim mode** (effective brightness == 1): hides all non-essential widgets (date, temperatures, CO2, humidity, wind, forecast, custom text). Only the clock and particle canvas (sky arc + precipitation) remain visible. Clock color switches from white (255,255,255) to dim gray (40,40,40) to compensate for the HUB75 driver's brightness curve difference vs BCM-based drivers.

**Fallback:** in `als` mode, before the first lux packet arrives (e.g. after boot), the time-of-day logic is used instead.

**Mode `manual`** — the `Brightness` light entity (monochromatic, `restore_mode: RESTORE_DEFAULT_ON`) directly drives the panel: its 0-100% slider maps to panel brightness via the output `write_action`. Turning the light OFF sets panel brightness to 0 (dark screen; widgets keep rendering). The last manual state and level survive reboot.

**Architecture:** the template output's `write_action` is the single actuation path — every light change (user action in `manual`, or the feedback call below in auto modes) flows through it: `brightness_255 = round(level * 255)`, `matrix.set_brightness()`, extra-dim flag, widget hide/show. In auto modes `update_brightness` computes the value and pushes it into the light via `make_call()` with `set_publish(true)`, `set_save(false)`, `set_transition_length(0)` — so the HA slider follows the auto brightness, flash is only written on real user actions, and there is no fade. A user slider change while in an auto mode applies momentarily and is overwritten by the next auto computation (~1s in `als`, up to 30s in `time`). The light has `gamma_correct: 1.0` — the panel applies its own brightness curve, and the default light gamma (2.8) would crush low brightness levels (e.g. 1% → ~0%).

Brightness conversion: HUB75 uses 0-255 scale. `brightness_255 = round(level * 255)`. Special case: brightness 1% maps to `1/255` to avoid excessive brightness at the lowest setting.

`update_brightness` runs on: boot (late priority, applies auto value over the restored light state), mode select change, each received lux packet (~1s), and a 30s interval.

### Weather Icons

27 Yandex weather icons (8x8 PNG) compiled into ESPHome at build time via `image: platform: file, type: RGB`. Dynamic icon selection uses the `mapping:` component to map HA text sensor values (e.g. `bkn_d`, `ovc_ra`, `skc_n`) to image IDs at runtime.

Image ID naming: `+` → `p`, `-` → `m` (e.g. `bkn_+ra_d` → `icon_bkn_pra_d`) for valid C++ identifiers.

Icons are rendered at full brightness (the original Python implementation dimmed icons to 70%; this was an artifact of the rendering pipeline and is intentionally not replicated).

## Home Assistant Integration

### Data sources

All data flows via ESPHome native API (no MQTT, no HTTP polling). HA entities are imported using `platform: homeassistant` sensors and text sensors. State updates are push-based (real-time). Exception: ambient lux arrives directly from the `presence-bedroom` device via UDP packet transport (bypasses HA).

| ESPHome ID | HA Entity | Type | Feeds |
|------------|-----------|------|-------|
| temp_inside | sensor.aqara_weather_02_temperature | sensor | temp_inside_label |
| temp_outside_measured | sensor.tuya_weather_02_temperature | sensor | temp_outside_label (measured) |
| temp_outside_provided | weather.yandex_weather (attr: temperature) | text_sensor | temp_outside_label (provided) |
| co2 | sensor.d1_co2_co2_scd30 | sensor | co2_label |
| humidity | sensor.aqara_weather_02_humidity | sensor | humidity_label |
| current_icon | sensor.fact_icon | text_sensor | weather_icon |
| wind_bearing | weather.yandex_weather (attr: wind_bearing) | text_sensor | wind_label |
| sun_state | sun.sun | text_sensor | brightness logic (`time` mode) |
| sun_rising | sun.sun (attr: next_rising) | text_sensor | sky arc (sun) |
| sun_setting | sun.sun (attr: next_setting) | text_sensor | sky arc (sun) |
| moon_rising | sensor.home_moon_rise | text_sensor | sky arc (moon) |
| moon_setting | sensor.home_moon_set | text_sensor | sky arc (moon) |
| precip_type | sensor.precipitation_type | sensor | particle system |
| precip_strength | sensor.precipitation_strength | sensor | particle system |
| wind_speed_real | sensor.wind_speed | sensor | wind_label, particle system (wind drift) |
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
| Brightness | light (monochromatic) | Panel brightness. Driver in `manual` mode; live display of auto value otherwise. OFF = dark screen (manual). |
| Auto Brightness Mode | select | Brightness mode: `time` (time-of-day + sun), `als` (lux curve), `manual` (light entity drives) |
| Ambient lux (internal) | sensor (packet_transport) | Internal; drives auto-brightness in `als` mode. Source: presence-bedroom `bh1750_lux` broadcast. |
| Curve Lux 1-5 | number (slider) | Lux band thresholds of the brightness step clamp |
| Curve Brightness 1-6 | number (slider) | Brightness levels per band (1-100) |
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
// Attach the display for direct-to-framebuffer overlay rendering (from on_boot)
void set_display(display::Display *display);

// Background snapshot sync: called from the LVGL on_render_ready trigger with
// the LVGL draw buffer (RGB565, stride in bytes). Copies it into the internal
// snapshot and redraws the current overlay pixels on top.
void on_frame_composited(const uint8_t *data, uint32_t stride_bytes);

// Overlay render tick rate (EMA), ~50 at the 20ms cadence
float get_render_fps() const;

// Legacy pixel source for the YAML-canvas rendering approach (esp32-hub75.yaml)
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

Paces the 20ms overlay render tick (main loop runs at 1ms). Each tick:
1. `refresh_sky_cache_()` — at most once per second: parses HA ISO datetime strings and precomputes sun/moon/horizon overlay pixels (cached in `sky_pixels_`)
2. Erase phase — restores background snapshot words over all `prev_pixels_` positions (exact RGB565 restore, glyph-safe)
3. Build phase — copies `sky_pixels_` and runs `update_particles_()` into `pixels_`
4. Draw phase — draws `pixels_` via `display->draw_pixels_at()` (1x1 RGB565), then `prev_pixels_` ← `pixels_`

If no display is attached (legacy YAML-canvas configs), `loop()` only maintains `pixels_` for `get_pixels()` and never draws.

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

- **RGB light mode** — fill entire display with a solid color (would be a direct framebuffer fill in the component, hiding all labels)
- **Debug borders** — toggle rectangles around widget bounds (would be drawn by the component into the framebuffer)

## Debugging

The `lvgl_screenshot` component serves the LVGL draw buffer over HTTP on port 8080 as a PNG image, useful for remote debugging without physical access to the panel. Note: it shows the LVGL-composited UI only — the particle/sky overlay lives in the HUB75 framebuffer and is not part of the screenshot.
