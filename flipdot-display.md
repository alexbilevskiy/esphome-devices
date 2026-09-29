# Flipdot Display (ESPHome)

A single custom ESPHome display component (`components/flipdot_display/`) for the **F15** flip-dot display module by Shenzhen Xinqidian Optoelectronics (XQD). The display hardware — the TM1824-driven modules, board architecture, the measured byte semantics and the power model — is documented in the [flipdot-stc8g/HARDWARE.md](https://github.com/alexbilevskiy/flipdot-stc8g) repo. Component-level facts repeated here from that doc:

- A module is 8x8 dots (64 dots); **every dot owns exactly one wire byte** (its coil drive duty — see the byte semantics in the hardware doc).
- The modules are wired two ways at once: the single-wire TM1824 chain runs through all modules in series (each module's last chip feeds the next module's first chip), and every module exposes an RS-485 DMX interface — the A/B bus is a daisy across modules, each module's decoder captures the 64-slot window at its factory address.
- Within a module the pixels are wired as a mirrored Z (rows scanned right-to-left); the modules are assembled into the display as a snake (block row 0 right-to-left, row 1 left-to-right, alternating, top-to-bottom; the top-right block is first in the chain). The component's blit and `module_order` implement exactly that order, so fonts, images, graphs and any other display library work out of the box.
- The coil stage runs the pulse-then-coast firmware on the per-quad STC8G MCUs (hardware repo README).

Two wire transports are implemented, selected by `connection` (the modules ship with both interfaces wired):

- **Single-wire (TM1824 chain)** — `connection: single_wire`: the chain driven directly from an RMT channel on `pin`.
- **DMX512 over RS-485** — `connection: dmx` (GPIO18/33 + external transceiver): the wired transport.

## Component

One component (`components/flipdot_display/`) produces the wire byte stream (mirrored Z within a block, blocks chained as a snake — see the intro) and pushes it out through one of two interchangeable transports, selected by `connection`:

- `connection: single_wire` — the folded-in RMT transport drives the chain directly from `pin`: bytes are encoded MSB-first, eight RMT symbols per byte, with the TM1824 datasheet bit timings centered, and the frame ends in the >= 200 us low reset that latches the chain. The wire frame is exactly one byte per dot (total_pixels bytes) plus the reset. The line parks at `eot_level` after the frame; RMT does not repeat frames.
- `connection: dmx` — the folded-in DMX512 transport (UART + external RS-485 transceiver): the same blit feeds the universe composition. Block k (chain order: the top-right block of row 0 first, then the chain snakes across block rows) is filled with the universe window of the module listed at `module_order[k]`: factory position n (1..8, the vendor's per-line limit) means slots `(n-1)*block_pixels+1 .. n*block_pixels` — the vendor's dense factory layout, one dot per slot; gaps between windows stay 0x00 (release). Without `module_order` the blocks default to the factory kit order (positions 1..N). The mapping is ordinal-only: a module re-addressed to a non-factory window with the vendor address writer cannot be expressed (bring raw addresses back if ever needed).

Both transports are feature-equal: identical fonts/images/lambda rendering, one shared render path — the diff against an own copy of the last transmitted frame (the committed bytes) drives dirty tracking and the throttled transitions in both modes.

DMX framing details: the ESP32-S2 drives an external RS-485 transceiver (MAX3485 class, 3.3 V): GPIO18 -> transceiver DI, GPIO33 -> transceiver DE (held high all the time — we are the only transmitter on the bus), transceiver A/B -> module A/B, common ground. Framing at 250 kbaud 8N2: start code 0x00 + all 512 slots; the IDF UART driver appends the TX break after the payload (30 bits = 120 us, spec minimum 88 us), and the decoder latches the slots on that trailing break — the idle gap until the next frame is the mark-after-break, so a single call is a complete valid frame. A module at base address `A` reads universe slots `A..A+63`.

Frame time: single-wire ~10 us per dot + 300 us reset (~1.6 ms for 2 blocks, `update_interval` down to 16 ms safe); DMX a full 0x00 + 512-slot universe at 250 kbaud = ~22.7 ms including the trailing break, so `update_interval` below ~25 ms is pointless over DMX.

## Current assembly

The 3x2 field (six modules, `cols: 3, rows: 2, module_order: [4, 2, 3, 5, 1, 6]`) running the clock — boot wipe + HH:MM + a switchable second row; the full sketch is `flipdot-display.yaml`. Top row right-to-left: kit **#4** (factory window 193..256, pulse-then-coast firmware), clock **#2** (65..128, pulse-then-coast firmware), **#3** (129..192, stock firmware); bottom row left-to-right: **#5** (257..320), **#1** (1..64), **#6** (321..384), all on the pulse-then-coast firmware. Dirty tracking keeps the wire silent while the picture is static, so the reflashed modules idle at ~zero current and pulse their dots only when something changes; the stock-firmware module keeps paying holding current. Modules are hot-swappable across positions — decoder windows travel with them.

## Configuration

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [flipdot_display]

display:
  - platform: flipdot_display
    id: flipdot
    connection: dmx    # or single_wire
    pin: GPIO18        # DMX: UART TX to the transceiver DI; single-wire: chain data line
    de_pin: GPIO33     # DMX only: transceiver DE (driver enable)
    cols: 2            # blocks per row
    rows: 1            # block rows
    block_width: 8     # optional, default 8
    block_height: 8    # optional, default 8
    on_level: 0        # byte for drawn (on) pixels; release byte -> light dots
    off_level: 255     # byte for background (off) pixels; driven byte -> dark dots
    module_order: [1, 2]  # DMX only; optional, factory positions, kit order by default
    update_interval: 50ms
    lambda: |-
      ...
```

Byte-to-look mapping is board physics, not configuration: `0x00` releases the dot (light), nonzero duty drives it (dark at `0xFF`) — the full byte semantics and the power model are in the hardware doc. The `on_level`/`off_level` pair only decides which of the two bytes the drawn content and the background get — i.e. which content is light and which is dark, and therefore which areas pay holding current.

| Option | Default | Description |
|--------|---------|-------------|
| `connection` | required | `single_wire` (TM1824 chain driven directly from an RMT channel) or `dmx` (DMX512 over RS-485 via a hardware UART and an external transceiver) |
| `pin` | required | DMX: UART TX, wired to the transceiver DI. Single-wire: the chain data line |
| `de_pin` | dmx only | Transceiver DE (driver enable); held high permanently (sole transmitter on the bus) |
| `cols` / `rows` | required | Block grid size |
| `block_width` / `block_height` | 8 | Pixels per block |
| `on_level` | 255 | Byte value for an "on" pixel |
| `off_level` | 0 | Byte value for an "off" pixel |
| `module_order` | 1..N | DMX mode: factory position (1..8) of every module in chain order (block rows chained as a snake; list length must be `cols * rows`; module n gets universe slots `(n-1)*block_pixels+1 .. n*block_pixels`; duplicates rejected) |
| `eot_level` | 0 | Single-wire mode: wire level after the transmitted frame |
| `rmt_symbols` | per variant | Single-wire mode: RMT memory block symbols (192 on ESP32/S2/S3/P4, 96 on C3/C5/C6/H2) |
| `concurrency` | 0 | Throttled switching: max dots whose changed bytes are sent in one frame; 0 = every transition is a single frame (classic behavior) |
| `step_interval` | 100ms | Pause between batches of a throttled transition (validated ≥ 25 ms for `concurrency > 0` in DMX mode) |
| `switching_effect` | none | Order of dots within a transition: `none` (chain order), `wave` (Chebyshev distance from the top-right corner), `random` (shuffled per transition) |

Standard display options (`rotation`, `pages`, `update_interval`, `auto_clear`) are supported.

## Throttled switching (concurrency)

A module's PSU is dimensioned for *all* dots flipping simultaneously (the vendor's 55 W / 5 V worst case; 5 A tolerated the same thing with noticeable sag). Once the pulse-then-coast firmware removed the idle draw, the peak only happens at flip time — and the flip load is fully determined on the ESP side: the STC8G stage pulses its bridges in parallel for exactly those dots whose duty changed in the received frame. So a frame whose diff touches ≤ N changed bytes caps the inrush at N dot-flips (~170 mA each → 5 A ≈ 29 dots per module).

With `concurrency > 0` the component stops transmitting a transition as one frame. `update()` renders the target frame, diffs it against the last transmitted bytes (the committed state), orders the changed dots per `switching_effect`, and `loop()` sends a batch of at most `concurrency` dots every `step_interval` until the transition is done. Every batch is one full frame in which only the batch's bytes changed relative to the committed state — the pulse-then-coast firmware exactly parallels the resulting bridge count with `concurrency`, and the dots that are already at their byte are not pulsated.

- The default `concurrency: 0` keeps the classic behavior byte-for-byte: diff-less full frame, single transmission. All remaining optic/rendering features work unchanged.
- The loop never blocks longer than one transmission (~23 ms DMX, ~1.6 ms single-wire): the transition is step-driven, so no watchdog activity and no API timeouts, unlike blocking inside `update()`.
- A stale in-flight transition is re-diffed on every new frame: render → diff → rebuild queue against the current committed bytes, so late-arriving frames carry no stale target state.
- The first batch of a new transition is sent immediately inside `update()`; while the transition stays active, further updates only refresh the target and the batches keep their `step_interval` cadence — one batch per update would run them at the update rate and defeat the throttling once `update_interval` drops below `step_interval`. An update whose diff comes out empty deactivates the transition. Transition start/done are logged (DEBUG), batches at VERBOSE.
- The single-wire transmitter is throttled identically: every batch is one full frame of the committed bytes, and since each dot owns its byte, unchanged bytes leave the duty latched (no re-pulse).
- Both modes cost slightly more frames at full transition speed; the flip-dot physics (and the STC8G's ≥ ~100 ms pulse slot) set the practical floor: `step_interval` ≥ 50 ms is recommended for a clean look, the validated minimum is 25 ms (one DMX frame at 250 kbaud).

## Example: moving pixel

The bench config runs the walking pixel as a render mode inside the display lambda, gated by a global and toggled with a template switch — the component keeps running its updates, no suspend involved:

```yaml
globals:
  - id: pixel_mode
    type: bool
    restore_value: no
    initial_value: 'false'

switch:
  - platform: template
    name: "Walking pixel"
    optimistic: true
    restore_mode: ALWAYS_OFF
    turn_on_action:
      - lambda: 'id(pixel_mode) = true;'
      - component.update: flipdot
    turn_off_action:
      - lambda: 'id(pixel_mode) = false;'
      - component.update: flipdot
```

The mode branch in the lambda:

```yaml
    lambda: |-
      if (id(pixel_mode)) {
        // single pixel running a serpentine path over the whole screen
        static uint16_t pos = 0;
        const int w = it.get_width();
        const int total = w * it.get_height();
        const int y = pos / w;
        const int xr = pos % w;
        const int x = (y % 2 == 0) ? xr : w - 1 - xr;
        it.draw_pixel_at(x, y);
        pos = (pos + 1) % total;
        return;
      }
      // ... other render modes (boot wipe, clock) ...
```
