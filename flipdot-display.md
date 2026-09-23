# Flipdot Display (ESPHome)

Hand-built monochrome dot display. Implemented as a single custom ESPHome display component (`components/flipdot_display/`) with two interchangeable wire transports selected by `connection`, so fonts, images, graphs and any other display library work out of the box.

Two protocols are implemented (the modules ship with both interfaces wired):

- **Single-wire (TM1824 chain)** — one component transport (`connection: single_wire`): the chain driven directly from an RMT channel on `pin`.
- **DMX512 over RS-485** (`connection: dmx`, GPIO18/33 + external transceiver) — currently the wired transport.

The board internals (per-module DMX decoder, per-dot MCU+H-bridge quads) are documented in "Board architecture"; the STC8G firmware replacement — the endgame for the power draw — is built and deployed on the clock's first two modules (see "STC8G reflash plan").

## Hardware model

The display's block drivers are **Titan Micro TM1824** chips (SOP8, four-channel constant-current PWM drivers, datasheet V1.2), daisy-chained on a single data line. Three 8x8-dot modules are chained together the same way as the chips inside them.

- Frame structure: D1...Dn + reset. Each chip captures its own **32 bits** (four 8-bit duty bytes: R,G,B,W, MSB first) from the front of the stream and retransmits the rest downstream with automatic waveform reshaping; a `>= 200 us` low reset latches all chips.
- The chain holds **48 chips (16 per module x 3 modules) x 4 channels x 8 bits = 1536 bits = 192 bytes**: exactly one byte per dot. Empirically verified with a walking pixel — pixel-perfect addressing (see "Byte semantics" for what a byte drives).
- The 900–1100 Hz (typ 1000 Hz) PWM on every output runs continuously for as long as a nonzero duty is latched — this is what produces the audible dot chatter, and why 0xFF still hums faintly (maximum duty is below 100%).
- **“全0码为关断”** — an all-zero duty byte turns the channel off, permanent and current-free. This is the only "off" the protocol has: any nonzero duty keeps the PWM running.
- The chip has no hidden commands or configuration registers — the only data are duty bytes. There is no "release coils and hold state" command.
- The ESP32 drives DIN directly with 3.3 V; the datasheet Vih is 3.5 V at VDD=5 V. It works within the tolerance of real chips but is out of spec — first suspect for any unexplained glitches.
- 1 bit per pixel. Each byte carries one dot's **coil drive duty** and re-latches on change; identical bytes in a repeated frame leave it untouched. The duty-to-color mapping is the locked-antiphase scheme described in "Byte semantics" below.
- Layout inside a module: pixels are wired as a mirrored Z — every row is scanned **right-to-left**, pixel 0 is the block's top-right corner, the last pixel is the bottom-left corner. Blocks/modules are chained as a snake: block row 0 right-to-left, row 1 left-to-right, alternating per row, top-to-bottom across block rows; the top-right block is first in the chain.

### Bit timing windows (datasheet, VDD=5 V)

| parameter | TM1824 spec | old WS2811 timings | current timings |
|---|---|---|---|
| T0h (high, "0") | 310–410 ns | 300 ns (below min) | 360 ns |
| T1h (high, "1") | 650–1000 ns | 1090 ns (above max) | 720 ns |
| bit period | 1.25–2.5 us | ~1.4 us | 1.25 us (360+890 / 720+530) |
| reset low | ≥ 200 us | 300 us | 300 us |

The chain tolerated the WS2811 timings inside the chips' tolerance; now everything sits in the middle of the spec windows.

## Board architecture (as traced)

Per-dot drive is fully individual — there is no row/column matrix. A module is built from **16 identical quads**, each owning 4 dots (16 × 4 = 64):

```
RS-485 A/B ──> TM512AC0 ──DO──(R_O?)──> DIN ─> TM1824#1 ─DOUT─> TM1824#2 ─ ... ─DOUT─> next module
                                             │ 4x duty-PWM state outputs (R,G,B,W)
                                             v
                                           STC8G ── IN1/IN2 + nSLEEP ──> 4x AT8837 ──> 4 coils
```

- **TM1824 = chain demux, not a coil driver.** Its RGBW outputs go to the paired STC8G's input pins (traced), carrying the latched duty as a ~1 kHz PWM state signal. This is why the chain chips exist at all: single-wire position addressing is decoded in hardware, and an 8051-class MCU cannot cheaply decode an 800 kbps RZ stream in software. The whole byte-semantics model measured on this display (0x00 = off, 0xFF = full, mid = locked-antiphase chatter, see "Byte semantics") lives in this duty signal.
- **STC8G (8051-class MCU, one per quad)** reads the 4 state pins and drives its 4 bridges. It does **not** touch the single-wire chain (traced: confirmed). Each MCU has a factory ISP header with VDD / GND / RX / TX broken out (see "STC8G reflash plan").
- **Full STC8G pin map** (traced, confirmed identical on all 16 quads of a module): TM1824 R/G/B/W → P3.6/P3.7/P5.4/P5.5 (open-drain duty outputs, read through the STC8G's internal 4 kΩ pull-ups); AT8837 IN1/IN2 → P1.0–P1.7 in bridge order; AT8837 nSLEEP (active high) → P3.2–P3.5 in bridge order; 16 of 18 I/O used, the remaining two are P3.0/P3.1 (the ISP UART).
- **AT8837 (single H-bridge per dot)** provides the coil polarity: forward / reverse / brake (coil shorted) / coast (outputs Hi-Z). Coil wiring per dot is untraced: a single bipolar winding between OUT1/OUT2 is what the part is made for; two unipolar coils (one per output) is electrically possible — open question.
- AT8837 facts: VM 2.7–12 V (coil supply), VCC 2.7–5.25 V (logic), 0.8 A continuous / 1 A peak, PWM up to 250 kHz, ~205 ns dead time, OCP 1.2 A, TSD 160 °C. Every control input has an internal ~100 kΩ pull-down: with the MCU off or in reset the bridges sit at IN1=IN2=0 = coast, coils float, dots hold magnetically. Hardware fail-safe for free. VM static draw is 0.3–0.8 mA awake vs 50–100 nA asleep — bridges are meant to be slept between pulses (almost certainly what the stock firmware does).
- The STC8G's interpretation (polarity choice, how a black dot is driven back to white, the boot behavior) is its firmware's business and is invisible from outside. The measured boot quirk (field white *and* drawing current until the first frame arrives, see Findings) is an active white drive state, not a passive release.

### DMX decoder: TM512AC0 (one per module, datasheet V1.5)

The single SOP8 next to the connector is a Titan Micro **TM512AC0** — DMX512 decoder with decode-and-forward. It receives the differential bus and re-emits the captured data as a single-wire RZ stream on its DO pin for "our 18-series and 19-series ICs" (TM1824 is 18-series; the stated 800 kbps rate = 1.25 µs bit time — exactly the timings measured on the chain).

Pins: DO (3) forward output; ADRO (2) / ADRI (5) address-write daisy output/input — the connector's "address" pin, ADRI has an internal pull-up; TST (4) factory test, internal pull-down; AI (6) / BI (7) differential inputs — AI has an internal pull-up, BI a pull-down, so a **floating bus idles at MARK** (explains the negative floating-bus probe). Differential thresholds: ±0.2 V, 70 mV hysteresis, 280 kΩ input impedance, Vcm up to 12 V.

Protocol requirements — all satisfied by the component's DMX transport (`connection: dmx`):

| requirement | spec | our driver |
|---|---|---|
| speed | adaptive 200–1000 kbps | 250 kbaud |
| reset (break) | ≥ 88 µs | 120 µs |
| mark-after-break | ≥ 8 µs | idle-high gap |
| start code | first field must be 0x00 | 0x00 |
| resets | ≥ 4 ms apart (max 250 fps) | 1 Hz |
| between frames | line stays HIGH | UART idle-high |

Addressing: the E2 address is a 0-based window start over the valid (post-start-code) fields, and the decoder captures and forwards **192 fields** from its address — not just its module's 64. The windows therefore overlap (decoder 1: fields 2..193, decoder 2: 66..257, decoder 3: 130..321), every module's chain consumes the first 64 bytes it sees, and the redundant identical re-transmissions from upstream decoders (delayed by ~0.64 ms per 16-chip module traversal) are harmless. That is what lets three decoders share one single-wire chain without coordination. (Forward timing/serialization not yet scoped.)

Address writing rides the same A/B lines with a proprietary pattern (dedicated writer tool only; the datasheet does not document it). After a successful write the decoder drives the forwarded chain at 25% white — the display flashes white on re-addressing. Our driver only sends DMX frames with start code 0x00 and cannot trigger the write mode.

DO → DIN link: per the TM512AC0 reference design the DO output reaches the chain input through a series resistor **R_O = 300–500 Ω** (same net as the connector's DIN pin). Accepted as the working explanation, not verified — and the reason a continuity check between DO and DIN fails: measure ohms, not beep.

Open probes:

- Coil terminals per dot: 2 (single bipolar winding) vs 3 (dual coils).
- Scope: DO streams of the three decoders vs the inter-module chain link — confirm collision-free interleaving.

## STC8G reflash plan (deployed on the clock's first two modules)

The coil stage behavior is decided by the STC8G firmware. Owning it enables **pulse-then-coast** drive: pulse the bridge on a dot's state change, coast otherwise. That makes *any* image zero steady current — the bistability the dots physically have but the stock firmware never uses (it holds coils energized continuously, which is the entire power draw story). This is the endgame of the power saga: the classic palette (dark background) stays, the bill collapses to flip transients. **Status: done on the kit's #4 module (all 16 quads; the original P=50 build) and on clock module #2 (all 16 quads; the P=100 build, see the pulse-width note in phase 3) — #4 runs as the clock's first position, #2 keeps its middle position; clock module #3 still runs the stock firmware.**

Established facts:

- The MCU is an **STC8G1K08-20/16PIN** (stcgal: Magic F754, BSL 7.3.13U, mfg date 2024-01) — the 18-I/O variant, TSSOP20, one per quad, 16 per module / 48 total. `uart1_remap=False`, `reset_pin_enabled=False` (P5.4 is a spare I/O).
- **A factory ISP header (VDD / GND / RX / TX) is exposed per MCU** — no soldering anywhere. P3.0/P3.1 (pins 11/12) are therefore dedicated to ISP, not consumed by the bridges. The factory programmed the MCUs through these headers; the flow is proven on this exact hardware.
- ISP lives in ROM, starts at every power-up and waits for the host handshake on P3.0/P3.1 (procedure: power off the module → start Download → power on). The ROM bootloader cannot be bricked — a bad flash is fixed by reflashing. During ISP all I/O is Hi-Z, so the bridges coast via their 100 kΩ pull-downs.
- **Read-back is impossible** on STC8 series (the BSL dropped it; see stcgal issue #7): the original firmware cannot be dumped, and the first Erase destroys it for good. Accepted — the required behavior is fully known from this display's measurements, we write from scratch.
- TSSOP20 pin map for the trace: 1–4 = P1.2–P1.5, 5–6 = P1.6/P1.7, 7 = P5.4 (spare I/O, not RST per the option bits), 8 = VCC, 9 = P5.5, 10 = GND, 11/12 = P3.0/P3.1 (ISP), 13–18 = P3.2–P3.7, 19–20 = P1.0/P1.1.
- Signal budget: 4 inputs (TM1824 RGBW) + 8 outputs (4 bridges × IN1/IN2) + up to 4 nSLEEP = ≤ 16 of 18 I/O. Fits even with nSLEEP on GPIO.

Toolchain (proven end to end on real hardware):

- **sdcc 4.5.0** (Debian apt; sdcc/sdar/packihx/sdranlib) + **FwLib_STC8** (`/opt/src/FwLib_STC8`, explicitly supports STC8G1K08) + **stcgal** (`/opt/src/stcgal`, dedicated `stc8g` protocol with RC trim during handshake).
- Firmware project: `/opt/src/flipdot-stc8g/` (own Makefile, builds the library sources + `src/main.c`, 8 KB code / 1 KB xram / 256 B iram). `make` produces `build/quad.hex` and copies it to `/mnt/c/Downloads/flipdot-stc8g-quad.hex` for flashing from the Windows host.
- Hello world **flashed and verified**: UART1 TX (P3.1 = header pin 12) prints a line per second at 115200 8N1 through the dongle. Erase / write / option-set all work; per-chip **UID** is read (`F754CA2008AB79` on the guinea pig) — usable as quad identity in logs.
- Clock decision: **no `SYS_SetClock()` / no IRC re-trim** — the chip runs on its power-on IRC default (stcgal measured 23.952 MHz, trim deviation −0.150%). The FwLib trim constants are per-chip; using the author's block would be a gamble. 24 MHz → 115200 baud error ~0.2%, well inside UART tolerance.
- stcgal also reports the chip's 4 KB IAP EEPROM — a future slot for per-quad calibration data (pulse width, bridge polarity) if quads ever need individualization.

Phases:

1. ~~Trace~~ — **done**: full pin map established (see "Board architecture"), confirmed identical on all 16 quads of a module.
2. ~~Bench ISP proof~~ — **done**: hello world flashed and verified over the header.
3. ~~Firmware~~ — **done** (`/opt/src/flipdot-stc8g/src/main.c`, SDCC): pulse-then-coast state machine. 4 duty inputs with internal ~4 kΩ pull-ups; ratio-based duty classifier — lows ≥ 3/4 of the window = black, ≤ 1/4 = white (0xFF's max duty is slightly below 100%, so the pin is mostly-low-but-not-always: level detection alone would misfire on the 1 kHz hum; the display only ever latches 0x00/0xFF, so nothing else exists in practice), in between = hold the previous classification; all changes confirmed within one sampling pass pulse their bridges **in parallel** (one 50 ms slot serves any number of simultaneous changes — serialized per-channel pulses queue up ~55 ms and miss flips at fast frame rates); then short brake (controlled decay) → coast → **bridge sleep** (VM static 50–100 nA asleep vs 0.3–0.8 mA awake; likely the stock behavior too). Pulse width: default 50 ms (empirical: 2 ms = the dot twitches without reaching the detent; 10 ms = flips work but are marginal/weak; 40 ms = already noticeably worse; >50 ms = no further gain). The ">50 ms no gain" plateau began as a bench impression and now has a direct data point: module #2 was reflashed with the default raised to 100 ms and runs next to module #4 at 50 ms — no observable difference in flip behavior, so doubling the pulse buys nothing. The build default is left at 100 ms (no downside found). The bridges always drive at full VM; duration and burst count are the only power knobs. **Burst mode**: a transition fires `burst_n` full-VM sub-pulses separated by a coast gap — console `N<n>` (1..10, default 1 = single pulse, unchanged behavior) and `G<n>` (0..200 ms, default 20 ms), nSLEEP held through the series, sleep once at the end. Motivation: on the failing dots a single pulse of *any* length (up to `P200`) produces no visible movement, while repeated pulses sometimes catch them (random count per pulse). Tested as an automated fix and **rejected**: bursts don't make those dots reliable either. Verdict: the limitation is mechanical — those discs' pivots need physical run-in, not a different pulse scheme (single, longer, repeated — all closed); the random zero-or-flip response per pulse is the mechanical signature. The burst stays as a bench/edge tool at the default N=1. Bridge polarity is runtime-toggleable; **no pulses at boot** (the initial classification is taken silently — a reflashed quad picks up the live image without disturbing it); re-enabling auto mode (`M`) re-syncs the tracked states with the current duty silently.
   **Sampling window — hardware-timed, the hard-won part**: the window is gated by Timer0 (exactly 4 ms = 4 PWM periods) and classification compares the low counts against the actual sample count. The first implementation counted a fixed 4096-iteration loop instead — under `--model-large` SDCC compiled it to generic-pointer XRAM calls and one window took ~50 ms, which phase-locked the state machine to the 100 ms frame rate: at a 300 ms update it half-worked, at 100 ms the middle dots of each quad starved (edge logs showed only two of four channels firing, and the two survivors were exactly the quad's first and last dots in walk order). Timer-gating made the window immune to codegen speed; the sampler runs at ~1–3 µs per pass with counters in `__data`.
   **Tuning console** over the ISP header UART (115200 8N1, line-based): `S` status, `P<n>` pulse ms, `B<n>` brake µs, `N<n>` burst count, `G<n>` burst gap ms, `X` polarity, `T` test cycle (all dots black→white through the same parallel-pulse path, phase markers printed), `F`/`R` raw single-direction probes (one pulse, IN1-high / IN2-high, all dots or one bridge `F2`; state not tracked), `D` dump one duty window (low counts of N samples + classification), `M` auto mode toggle, `V` verbose edge logging, `?` help. Final values are hardcoded defaults (P=100 ms, B=200 µs, burst N=1 / G=20 ms); the 4 KB IAP EEPROM is a candidate for per-quad calibration if ever needed. Bench rule: test with a static frame on the ESP side ("DMX all 0x00") — a running animation re-pulses dots by duty edges right after every manual test and garbles the observations.
4. ~~Verify~~ — **done**: clean walking pixel at a 100 ms update on the fully reflashed module, then on the full 3-module display; the clock runs (boot wipe + HH:MM, one frame per minute by dirty tracking). A steady-state clamp-meter reading of the whole module remains the only unverified item.
5. ~~Rollout~~ — **done for the kit's #4 module and clock module #2**: all 16 quads each flashed over their ISP headers (conveyor). Clock module #3 keeps the stock firmware until the same treatment. Modules are hot-swappable across clock positions — decoder windows travel with them. An ESP-side periodic full repaint (invert-all frame + real frame) remains an option to heal any missed edges on static images.

Risks: original firmware is unrecoverable (accepted); a half-flashed quad = 4 dead dots until reflashed (ROM unbrickable); never set the lock/encrypt ISP options.

## Component

One component (`components/flipdot_display/`) produces the wire byte stream (mirrored Z within a block, blocks chained as a snake — see Hardware model) and pushes it out through one of two interchangeable transports, selected by `connection`:

- `connection: single_wire` — the folded-in RMT transport drives the chain directly from `pin`: bytes are encoded MSB-first, eight RMT symbols per byte, with the TM1824 datasheet bit timings (see the table above), and the frame ends in the >= 200 us low reset that latches the chain. The wire frame is exactly one byte per dot (total_pixels bytes) plus the reset. The line parks at `eot_level` after the frame; RMT does not repeat frames.
- `connection: dmx` — the folded-in DMX512 transport (UART + external RS-485 transceiver): the same blit feeds the universe composition. Block k (chain order: the top-right block of row 0 first, then the chain snakes across block rows) is filled with the universe window of the module listed at `module_order[k]`: factory position n (1..8, the vendor's per-line limit) means slots `(n-1)*block_pixels+1 .. n*block_pixels` — the vendor's dense factory layout, one dot per slot; gaps between windows stay 0x00 (release). Without `module_order` the blocks default to the factory kit order (positions 1..N). The mapping is ordinal-only: a module re-addressed to a non-factory window with the vendor address writer cannot be expressed (bring raw addresses back if ever needed).

Both transports are feature-equal: identical fonts/images/lambda rendering, one shared render path — the diff against an own copy of the last transmitted frame (the committed bytes) drives dirty tracking and the throttled transitions in both modes.

## DMX512 interface

Each 8x8 module also exposes an RS-485 DMX512 interface: the 8-pin connector carries signal A/B (differential) and everything else is supply/ground/address. One SOP8 decoder per module — a Titan Micro **TM512AC0** (see "Board architecture") — listens on the bus.

**Bus rules from the vendor doc** (the doc is thin, treat with care):

- One module occupies **64 universe channels, one channel per dot**; 8 modules max on one line (8 x 64 = 512 slots — a full universe offset), so factory base addresses are dense: **1, 65, 129, ...** for chained modules.
- Addresses are written at the factory and changeable with a dedicated "address writer" tool; this project only uses the factory layout, so the display maps each block by the module's factory position via `module_order` (see Component).

**DMX transport** (folded into `components/flipdot_display/`): the ESP32-S2 drives an external RS-485 transceiver (MAX3485 class, 3.3 V): GPIO18 -> transceiver DI, GPIO33 -> transceiver DE (held high all the time — we are the only transmitter on the bus), transceiver A/B -> module A/B, common ground. Framing at 250 kbaud 8N2: start code 0x00 + all 512 slots; the IDF UART driver appends the TX break after the payload (30 bits = 120 us, spec minimum 88 us), and the decoder latches the slots on that trailing break — the idle gap until the next frame is the mark-after-break, so a single call is a complete valid frame. A module at base address `A` reads universe slots `A..A+63`.

Current bench: the 3x2 field (six modules). Top row right-to-left: the kit's **#4** (factory-marked position 4; decoder window **193..256**) sits in the first (rightmost) position with **all 16 quads reflashed** with the pulse-then-coast firmware (see "STC8G reflash plan"; the duty classifier's window is hardware-timed — Timer0-gated 4 ms — after a counted sampling loop compiled to ~50 ms under `--model-large` and phase-locked the state machine to the frame rate). Clock module **#2** (65..128) also runs the pulse-then-coast firmware now (the P=100 build); module **#3** (129..192) still runs the stock firmware. Bottom row left-to-right: **#5** (257..320), **#1** (1..64) and **#6** (321..384), each with all 16 quads reflashed with the pulse-then-coast firmware. Decoder windows travel with the modules. The display config is `cols: 3, rows: 2, module_order: [4, 2, 3, 5, 1, 6]` (block 0 = the top row's rightmost; the chain snakes: block row 0 right-to-left, row 1 left-to-right; physical module order: top row right-to-left 4, 2, 3, bottom row left-to-right 5, 1, 6; factory windows 193..256 / 65..128 / 129..192 / 257..320 / 1..64 / 321..384) running the clock: boot wipe + HH:MM (top row) + a switchable second row at a 1 s interval — the component's dirty tracking transmits only when something changes, so the reflashed modules pulse dots once a minute (clock) plus whatever the second row changes and idle at ~zero current between them (the stock-firmware module keeps paying holding current). The second row is the mutually exclusive `Date`/`Ant animation` switch pair (turning one on turns the other off — the switch entity included, so the HA UI stays in sync; if both ever end up on, the animation wins; both off = a plain black second row): the date (`29 Sep` — day and month printed separately with only the cell sidebearing between them, raised flush to the row top and the field's left/right edges so the trailing p fits whole) or two Langton ants, launched as a 180°-rotated pair so the pattern stays centrally symmetric forever — each ant inverts the cell it stands on (white → turn right, black → turn left) and steps forward with toroidal wrap, so one step inverts exactly 2 dots. The step interval is the runtime-adjustable `Anim Step` number (1–60 s, default 10 s); the ant state lives in function-local statics and freezes when the switch goes off, resuming (with one overdue step) when it comes back. The walking-pixel lambda is kept in the YAML comments for bench work. Updates run at 25 ms — the DMX frame floor — but dirty tracking keeps the wire silent while the picture is static. Animated modes advance on their own 200 ms gate inside the lambda: that is the STC8G's real cadence floor (duty detection ~12 ms + the 100 ms coil pulse); faster steps would be filtered by its duty debounce and never flip a dot. The ant animation gates itself slower instead, through the `Anim Step` number. The render lambda is runtime-switchable: a `Clock` switch (off = the field settles at the project's plain black background; on the stock-firmware module #3 that black field pays holding current, the reflashed ones pulse once and sleep), a `Walking pixel` switch (serpentine single dot over the whole screen, one hop per gate step), the `Date`/`Ant animation` pair described above (the lambda reads their switch states directly — template switch restore does not fire actions, so no separate globals to desync after a reboot), and manual pixel exercise — `Exercise X`/`Exercise Y` number inputs plus an `Exercise pixel x10` button that toggles that dot 10 times (one per gate step, ends black; the exercise overrides the walking pixel and the clock for its run, an active boot wipe finishes first). A `Boot wipe` button re-runs the boot pattern on demand: two held phases, all white then all black (3 s each), so both dot ends get visited. Each phase is held longer than its own full-field transition (~2.4 s at `concurrency: 8`) because the component rebuilds its transition queue on every update — a phase that ends early aborts its own sweep mid-way. The white first phase also matches the TM1824 power-on field, so a cold boot shows no flash. The DMX probe buttons (uniform frames, raw walking pixel, window sweep) were removed from the YAML; if a raw DMX rig is ever needed again, the display's debug probes (see below) cover the uniform frames.

No longer test-rig-only: the display component drives the bus directly (the currently wired transport; `connection: dmx` in the YAML). DE is held high permanently — we are the only transmitter on the bus, and the decoder idles safely at mark between frames.

### Signal-loss probes (closed: no release-on-signal-loss behavior)

The "release everything without touching the frame state" hypothesis is closed. The decoder latches on trailing break, so state-touching frames and "no signal" were the only separable things. Probes, all with the clamp meter, all negative (current unchanged, picture unchanged):

1. **Plain idle** — black frame, then the bus at mark (transceiver driving, DE high) for minutes: no drift.
2. **Long break** — forced breaks far beyond spec (up to 1 s, clocked at a lowered baud to beat the 255-bit IDF break cap): no effect.
3. **Floating bus** — DE low (`bus_off()`), the closest emulation of the cable pulled: no effect.

The module's decoder holds its data indefinitely; it has no fail-safe release. Probe buttons were later removed from the YAML; the probes remain available as debug methods on the display component (see "Debug probes" below) — power switching was removed, but the probes stay for any future rig.

Frame time: single-wire ~10 us per dot + 300 us reset (~1.6 ms for 2 blocks, `update_interval` down to 16 ms safe); DMX a full 0x00 + 512-slot universe at 250 kbaud = ~22.7 ms including the trailing break, so `update_interval` below ~25 ms is pointless over DMX.

The component keeps the last transmitted frame and sends a new one only when something changed. Between transmissions the bus is silent, but the driver keeps running the latched coil drive, so the current draw after a frame is set by that frame's contents, not by the wire. Power rule: **an all-0x00 frame is the only free state — it releases the coils entirely (white field, no significant current). Any nonzero byte keeps the coil stage energized.** Note that a white *look* is not free: any small nonzero byte (0x01) holds the field white but draws nearly as much current as 0xFF. To save power a static image must be latched at exactly 0x00 — i.e. white background with black content paying current only for the drawn dots.

## Power switching (module rail cut) — tested, rejected

A power switching scheme (N-FET low-side on the module rail via `power_supply`) was implemented and tested with the aim of a zero-idle-current display of any color. It does not work for this hardware and was removed:

- With the rail **off**, the magnetic picture holds indefinitely — the dots are truly bistable across a power cut.
- With the rail **back on**, the chips power up with zeroed duty registers (the "all zeros = off" default), which on this board means **the whole field drifts to white** during the rail settle time. Every power-up therefore wipes the picture, and the subsequent full frame re-drive is visible as a white→black→image flash.
- Flashes of any frequency were rejected; a full-frame delta-less rewrite on every update defeats the purpose anyway.

Practical consequence: power switching is unusable here, so the rail stays on permanently and the palette decides the bill. The release byte (0x00) is the only free byte; whichever content area is latched at a nonzero byte pays holding current. The current palette (classic look): dark background latched at 0xFF (paying), light content at the release byte. A light-background palette is technically free but was rejected (the light-on-dark look is the wanted one).

## Byte semantics on this display

The TM1824 chip itself only sets a duty per channel (open-drain output, 1 kHz PWM, "all zeros = channel off"); the coil polarity / bridge logic lives in the STC8G + AT8837 stage (see "Board architecture") and was mapped empirically. On this board the net behavior is that of an H-bridge in locked-antiphase PWM: the byte is a duty `d = byte/255`, the coil is driven toward **black during `d` of each PWM period and toward white during the rest** (net force toward black = `2d-1`), and it keeps doing this for as long as the value is latched:

- `0x00` — special-cased "all off": coils released, detent holds the dots, no significant current (see the measurement caveat in Findings). A field of any other value costs current (measured: 0x01 eats nearly as much as 0xFF; mid-range values eat MORE than 0xFF — reversal spikes), so free constant-white is only the exact-0x00 latch.
- `0x01`–`0x65`: the net force is fully toward white — the dots flip white, but the drive continues (~full current, faint hum/chatter toward the top of this zone).
- Mid-range (`~0x80`–`0xC0`): net force hovers at zero; every dot is battered in both directions each period and flips black↔white asynchronously, forever. Maximum current draw. It never settles, is independent of the value's history (no hysteresis) and is not affected by repeated identical frames.
- `0xFE`–`0xFF`: net force fully toward black — the dots flip black and hold; constant current, faint magnetostriction hum.

There is **no sub-threshold / "park" byte** (hysteresis scans, repeat-spam probes and long idle all tested, all negative), and the wire idle level is irrelevant (`eot_level` low/high, RMT does not repeat frames). Every transmitted frame drives every dot to the value's end of the scale first (dots flip to whichever end the duty is closer to) and then the drive continues per the rule above.

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

Byte-to-look mapping is board physics, not configuration: `0x00` releases the dot (light), nonzero duty drives it (dark at `0xFF`, see "Byte semantics"). The `on_level`/`off_level` pair only decides which of the two bytes the drawn content and the background get — i.e. which content is light and which is dark, and therefore which areas pay holding current.

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

For a hard freeze of the current frame the `component.suspend`/`component.resume` pair on the display component still works as before — it stops the updates entirely and the driver keeps the last latched levels, so the dots stay put; the shipped config just no longer uses it.

## Findings: power draw and driver behavior (settled)

The "idle power" question was investigated with a raw test rig and is closed. It was never an idle-line problem at all. Full value map: 0x00 = free white release; 0x01 eats nearly as much as 0xFF (white with active drive); mid values chatter forever with maximum current (locked-antiphase dithering — see "Byte semantics"); 0xFE–0xFF = held black with constant current. Measurement caveat: all current readings were taken on the module's shared PSU, which also fed the ESP32 and the buck converters — small draws were indistinguishable, so "zero current" throughout this document means "no significant coil-side draw". Settled facts:

- **The only free state is an all-0x00 (white) frame.** Every nonzero byte keeps the coil stage energized — even a white-look field latched at 0x01 draws nearly the 0xFF-level current. That is where the original "the display eats power" came from: a black background holds the whole field at a nonzero byte.
- **No "hold without driving" state exists in the protocol.** Any byte above 0x00 drives the coil continuously; there is no sub-threshold value that keeps the state at zero current. Pulse-then-release (impossible for black; white's free release is the exact-0x00 latch) and parking schemes were tested and closed: descending scans from black, repeated identical frames (spam), and long idle (an hour — the driver never sleeps).
- **The dots themselves are physically bistable**: cut the display's power in any state — white or black — and the picture holds. No springs, magnetic detent. The holding current seen at any nonzero byte is pure driver electronics (H-bridge still switching), not a physics requirement.
- The idle line level (GPIO low vs high) changes nothing, and the ESP's RMT output is clean: no loop mode, no frame repetition, the channel parks at `eot_level` after each frame. Wire integrity matters only at latch time (pixel-perfect addressing was verified); after the latch the chip PWMs from its internal register. One real margin concern: the ESP drives DIN with 3.3 V against a datasheet Vih of 3.5 V (see Hardware model) — empirically fine, but out of spec.
- A MOSFET cutting the coil supply rail between updates was implemented on top of `power_supply` and **rejected after testing**: the picture survives a power *cut*, but every power-*up* resets the chips to the all-zero (release) state, which lets the field drift to white during rail settle — each cycle produced a white→black→image flash, and flashes of any frequency were unacceptable. See "Power switching".
- Redesigning the drawing mode is also rejected: the accepted scheme keeps the rail permanently on and the classic palette — background at the driven byte (paying holding current), content at the release byte (free). The swapped (free-background) palette was tried and rejected aesthetically; power switching was tried and rejected for the flash-on-power-up problem.
- **DMX512 mode does not change the power picture.** With the bus protocol in place, expectations were that the module's own DMX decoder might wrap dots in pulse-then-release semantics (classic flip-dot DMX behavior: flip on change, then free). It does not: whatever the decoder outputs ends up latching duty in the TM1824 chain the same way as the single-wire protocol. A black field held through the DMX path draws the same holding current as a black field latched through DIN. Implemented and verified working, no bistability gain.
- Quirk observed and accepted: with the display board powered but no frame ever received, the field shows white and draws current (chip power-on default differs from a released coils state); the first transmitted frame clears it.

Bottom line: current is paid for by every dot latched at a nonzero byte, and mid-range bytes cost the most. The only free byte is the 0x00 release; the palette decides what gets it. The accepted trade-off: classic look (dark background at 0xFF paying, light content at the release byte), rail always on, no power switching (closed: flash on every power-up). All switching and parking schemes are closed (see "Power switching").

## Debug probes

The former raw test rigs (`components/flipdot_raw/`, `components/flipdot_dmx/`) are folded into `components/flipdot_display/` as its two transports; their probe APIs remain as debug methods on the display component, each guarded by the active connection type (a wrong-mode call logs an error and does nothing):

- `debug_uniform_frame(value)` — one frame with every wire byte at `value`; the committed state is synced, so dirty tracking stays correct afterwards. In DMX mode the uniform bytes land in the module windows; gaps stay 0x00. Cancels an in-flight throttled transition.
- `debug_long_break(ms)`, `debug_bus_off()` / `debug_bus_on()` — DMX mode only (long-break and floating-bus signal-loss probes, see above).
- `debug_pin_low()` / `debug_pin_high()` — single-wire mode only: park the data line at a level.

Nothing calls them from the shipped config; they are there for future bench automation.

Test rig heritage: the uniform-frame buttons (0xFF / 0x00 / 0x01 / 0x65, Spam x10, Idle pin LOW/HIGH) and the DMX probe buttons (uniform frames, raw walking pixel, window sweep) were all removed from the YAML once the display took over; the C++ APIs above are what remains of both rigs. The display section is active and drives the chain over DMX.
