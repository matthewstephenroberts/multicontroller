# LEGO color-sensor emitter

The board can pretend to be a **LEGO Powered Up Color Sensor** (LPF2 type `0x3D`) on a
dedicated UART, and pack any of its live sensor readings into the sensor's RGBI channels so
a SPIKE Prime / Pybricks program can read them with `color_sensor.rgbi()`.

The protocol engine in `firmware/components/lego_emit/lpf2.*` is an ESP-IDF port of the
Arduino `PoweredUp` library from the `LegoSensorColorGoodPerfect` project; the handshake,
mode descriptors, and combo-mode framing are unchanged. `lego_emit.*` adds the configurable
bit-packer, the value cache, and the FreeRTOS task. See that project's `docs/handshake.md`,
`docs/combo-mode.md`, and `docs/data-frames.md` for the wire-level protocol details.

## Device profiles

The emitter has a selectable **device profile** (`profile` in the config / a dropdown in the
web card) — one active at a time:

| Profile | Type | Direction | What it does |
|---------|------|-----------|--------------|
| **Color Sensor** | `0x3D` | board → hub | packs readings into RGBI / passthrough (everything below) |
| **3×3 Light Matrix** *(currently disabled)* | `0x40` | hub → board | receives pixel writes; renders them on the onboard TFT + the web grid |

> **3×3 Light Matrix is currently disabled** in the web UI's profile dropdown (`LEGO_PROFILE_MATRIX` is commented out in `LegoConfigForm.tsx`). The implementation below exists and is believed correct against the protocol docs, but it hasn't been validated against a real Technic 3×3 Color Light Matrix — we don't currently own one. It needs funding to purchase the actual LEGO hardware before this can be tested and re-enabled — see the [README's "Support MultiController development" section](../README.md#support-multicontroller-development), or open an issue if you already have one and can help test.

**Why a profile and not just modes:** the hub binds its high-level API — and the SPIKE app
routes its blocks — by the **announced LPF2 type byte**, not by the mode list. A `0x3D` device
only ever gets read (combo polling); it never receives a WRITE, so a colour sensor can't be
driven by the hub. To receive a 3×3 matrix the device must *announce* as the Technic 3×3
Color Light Matrix (`0x40`). Switching profile re-runs the handshake live (no reboot).

### 3×3 Light Matrix (`0x40`) — currently disabled, needs funding to test

`build_matrix_modes()` declares the genuine matrix mode set (`LEV O / COL O / PIX O / TRANS`).
**PIX O** (mode 2) is the writable 9-pixel mode; its WRITE callback decodes each byte as
`(brightness << 4) | colour_id` (the Pybricks/SPIKE light-matrix format, colour ids 0–10),
maps it to RGB, and fans out to:

- the **onboard TFT** — `display_show_matrix()` renders a 3×3 colour grid for ~30 s;
- the **frontend** — a `lego_matrix` BLE event (9 `#rrggbb` cells) drives the live grid in the
  web LEGO card.

Drive it from the hub with Pybricks `ColorLightMatrix(Port.X).on([...])` or the SPIKE app's
3×3 Color Light Matrix blocks. *(A physical WS2812 output is a natural extension — add a
strip-write in `matrix_pix_callback`.)*

## Wiring

A LEGO PUP connector carries power, an analog ID line, and a UART pair. For emulation only
the UART matters:

| LEGO connector | ESP32-S3        | Notes                                  |
|----------------|-----------------|----------------------------------------|
| Hub RX  (M1)   | `tx_gpio` (2)   | sensor → hub                           |
| Hub TX  (M2)   | `rx_gpio` (1)   | hub → sensor                           |
| GND            | GND             | common ground is required              |

Pins, UART port, baud, and the LPF2 type byte are all editable in the web UI ("LEGO sensor
emitter" card) and seeded from `firmware/main/board_config.h` (`BOARD_LEGO_*`). The default
is **UART2 on GPIO2 (TX) / GPIO1 (RX)** — UART1 is the auxiliary sensor bus, so the emitter
gets its own port. The handshake runs at 2400 baud and switches to 115200 for operation.

## Encoding

The Color Sensor's `RGB I` mode (mode 5) carries **four 16-bit values** = 64 bits. The
firmware packs the configured fields **LSB-first** into a 64-bit word and splits it:

```
word = R | (G << 16) | (B << 32) | (I << 48)
```

Each field maps one decoded sensor value into a run of bits:

```
raw   = round((value - offset) / scale)      # clamped to the field's bit width
decode: value = raw * scale + offset
```

- `bits` is any width from 1 to 16 (1/2-bit widths suit boolean flags and tiny codes like the gamepad dpad); the total across all fields must be ≤ 64.
- `signed` encodes/decodes as two's-complement within `bits`.
- Pick `scale`/`offset` so the value range fits the bits — e.g. a 0–1275 mm distance in
  8 bits with `scale = 5` (`raw = mm/5`, 0–255), or a −40..85 °C temperature in 8 bits with
  `scale = 1, offset = -40, signed = false`.

The web form shows a live **bits-used / 64** meter and generates a ready-to-paste Python
decoder for the exact layout you configured.

## Value conversions, calibration, and auto-scale

Each sensor has a **convert** mode (`transform`) that maps its raw driver output into standard
measurements, applied in the read path so the dashboard, display, and LEGO emitter all see the
derived values:

| Sensor | Modes | Outputs |
|--------|-------|---------|
| qmi8658 (IMU) | `imu_orient` (Madgwick) / `imu_tilt` | roll/pitch/yaw ° — yaw drifts (no magnetometer); tilt = accel pitch/roll, no drift |
| tcs34725 (colour) | `col_rgb255` / `col_hue` / `col_lego` | 0–255 R/G/B · hue/sat/val · LEGO colour id 0–15 |
| as7341 (10-band spectral) | `raw` / `as_lego` | F1–F8 + Clear + NIR counts · LEGO colour id 0–15 via spectral matching |
| generic | `dist_mm` / `dist_cm` | distance in mm / cm |

The **AS7341** is an 11-channel spectral sensor (8 visible bands F1–F8 + Clear + NIR, I2C
0x39). Its `as_lego` mode runs a chromaticity-weighted spectral match against a reference
table for far better colour discrimination than the 3-channel TCS34725; **Calibrate** with a
white tile captures the per-channel white reference for accuracy.

**Time-of-flight distance sensors** (all I2C, output `dist` in mm; use `dist_cm` to convert,
`dist_mm` to add a zero-offset calibration):

| Type | Sensor | Notes |
|------|--------|-------|
| `vl53l1x` | bare ST VL53L1X (e.g. TOF400C breakout) | full ST init (long range, ~50 ms budget, continuous); addr 0x29 |
| `tof10120` | TOF10120 module | onboard MCU; 2-byte read at addr 0x52 |
| `tofi2c` | TOF050C/050F/0200C/0400C/TOF400C module boards | simple read; default addr 0x29, register 0x00, 2 bytes — adjust addr/register/byte-order/scale in the sensor's **recipe** if your board differs |

VL53L5CX (8×8 multizone) is not yet supported — it requires an ~84 KB firmware upload to the
sensor at boot and returns 64 zones rather than a single distance.

**Calibration** — for the colour, IMU, and distance modes a **Calibrate** button captures a
reference into NVS (colour: white tile in front → white balance; IMU: hold still → gyro bias;
distance: present zero target → offset). It persists across reboots.

**Auto-scale** — a LEGO field is edited as a **min/max** value range; the firmware encode
params (`scale`/`offset`) are derived for the chosen bit width (`scale = (max−min)/(2^bits−1)`).
The **auto** button seeds min/max from the value's catalogue default. The field shows the
**step** (value change per encoder count) so you can size bits vs. resolution.

**Colour → RGBI preset** — one click creates 4×16-bit fields aligned **red→R, green→G,
blue→B, clear→I** from a colour sensor in `raw` mode, so a colour sensor maps naturally onto
the LEGO colour sensor's channels. (Channel mapping is positional: each 16-bit field fills the
next channel; the field editor shows which channel — R/G/B/I — each field lands in.)

**Live view** — with streaming on, the LEGO card shows, per field, the live `value → raw
(channel)` and the resulting `R/G/B/I` the hub receives, so you can confirm the encoding
matches the hub decode.

## Targeting COLOR/REFLT directly (without full colour passthrough)

By default a field's **target** is **RGBI word** — it bit-packs into the 64-bit RGBI payload,
readable via `color_sensor.rgbi()` / `device.read(5)` / the "red/green/blue light" advanced
blocks. But the emulated sensor's **COLOR** (mode 0) and **REFLT** (mode 1) bytes are separate
values the hub can request on their own — `color()` / `reflection()` (and their word-block
equivalents) read *those*, not the RGBI word, so a bit-packed field is invisible to them no
matter where it sits.

Set a field's **target** to **COLOR byte** or **REFLT byte** to drive that value directly
instead of packing it into RGBI — useful for exposing one value through the simplest possible
hub-side reporter (no `rgbi()`/unpacking needed at all) without setting up full colour
passthrough (below). COLOR/REFLT targets are always a single byte (0–255, via the same
scale/offset as any other field) and don't consume the 64-bit RGBI budget. Only one field per
target is meaningful — if more than one targets the same byte, the last one configured wins.

## Colour passthrough (native hub Color Sensor view)

By default the emitter packs arbitrary data into the RGBI channels, so the hub's built-in
*Color Sensor device view* (colour swatch + reflection + RGB) won't look meaningful — that
view interprets the channels as a real colour against the sensor's 0–1024 range. Read your
encoded data with `color_sensor.rgbi()` in code instead (or target COLOR/REFLT directly for
just those two values, above).

To make the **native hub view and `color()` / `reflection()` / `rgbi()` work properly**, set a
colour sensor's convert mode to **`colour + reflect + RGB`** (`col_full` for TCS34725,
`as_full` for AS7341 — outputs `colour, reflect, r, g, b` with RGB scaled 0–1024, plus a 6th
value `clear`: the raw, uncalibrated broadband count), then in the LEGO card set **colour
passthrough** to that sensor. The emitter then drives COLOR (official SPIKE colour id), REFLT
(reflected %), and all 4 values of RGB I — R/G/B (0–1024) **and** I (the raw `clear` count) —
from the sensor instead of the bit-pack fields, so `rgbi()`'s 4th value is real data rather than
always `0`. **Calibrate** the colour sensor against white first for accurate ids/RGB.

**Colour → native fields** (in the field editor, when colour passthrough is off) builds the same
5 values (colour/reflect/r/g/b) as ordinary fields — `clear` isn't included by default since it
doesn't fit the RGBI word without also using up the I channel, but you can add it yourself via
**Quick assign**'s I slot or the detailed field editor if you want it there too (proportionally
auto-scaled like any other raw count, since unlike the other 5 it isn't a value the hub
interprets in one fixed native format).

When the hub enables combined mode it sends a `CMD_COMBO_SET` listing the `(mode, dataset)`
pairs it wants (e.g. COLOR + REFLT + RGB I). Per the LPF2 spec the device must reply with
exactly those datasets concatenated **in request order** in a single MODE_0 frame, each sized
by its mode's format, padded to the next power-of-2 — with **no per-mode prefix**. The emitter
parses the pairs (`getComboPairs`) and builds the reply to match, so `color()`, `reflection()`
and `rgbi()` all decode correctly. (Previously it always replied with a fixed 2-byte prefix +
RGBI regardless of the request, so COLOR/REFLT landed at the wrong offset and never changed.)

**Colour-match visualisation**: set an AS7341 to **`colour match scores`** (`as_dist`) — the
dashboard shows a live bar per official colour (black/white/red/yellow/green/light-blue/blue/
violet, 0–100), highlighting the closest match, so you can see how confident the spectral
classification is and where two colours are competing.

## Reading it on the hub

On SPIKE Prime, `color_sensor.rgbi()` returns the four raw channels (combo mode). Feed them
to the generated decoder:

```python
from hub import port
import color_sensor

def decode(r, g, b, i):
    w = r | (g << 16) | (b << 32) | (i << 48)
    # ... per-field extraction generated by the web UI ...
    return out

while True:
    r, g, b, i = color_sensor.rgbi(port.A)
    print(decode(r, g, b, i))
```

Pybricks: `PUPDevice(Port.A).read(5)` returns the same four values for mode 5.

## How it runs

- `lego_emit_init()` (called from `main.c`) creates a task pinned to core 1; it is idle until
  a config with `enabled: true` is applied.
- The scheduler's reading callback fans out to `lego_emit_on_reading()`, which caches the
  latest values per sensor id.
- The task services the LPF2 handshake/keepalive (≥ 20 Hz), and on each hub poll packs the
  cache into the RGBI payload — via the combo callback for `rgbi()`, or a single mode-5
  frame if the hub selects `RGB I` directly.
- Saving a changed `lego` object over BLE re-applies it live (`lego_emit_apply()`); no reboot.

## Debug logging

Two toggles in the "LEGO sensor emitter" card (both runtime, no reflash, persisted in NVS) —
watch the serial console with `idf.py monitor` or `./scripts/monitor.sh`:

- **events** — *simple*: logs just the high-level handshake/runtime events — `[EVT] hub SELECT
  -> mode N`, `[EVT] hub COMBO SET/RESET`, `[EVT] hub WRITE -> mode N`, plus a 2‑second
  `link: mode=N combo=B data-reqs=R …` line showing the current mode and how often the hub is
  polling for data. Use this to see **when the hub switches modes and asks for callbacks**
  without the byte spam.
- **debug** — *verbose*: the full byte-level LPF2 trace (`[TX]`/`[RX]` frames) on top of the
  events, for protocol-level debugging.

From connect, the colour profile starts in the hub's **standard 6‑pair combo**
(COLOR + REFLT + RGB I — the exact `CMD_COMBO_SET` a SPIKE hub issues at program start) and
streams that combo frame every tick, so colour, reflect **and** RGB are live in the hub's
device view immediately. The hub still drives the combo shape: any real `CMD_COMBO_SET` it
sends overwrites the seeded one, and `CMD_COMBO_RESET`/`SELECT` drop back to single-mode data.

Connection lifecycle is logged at INFO even with debug **off**, so you always see progress:

```
I lego_emit: emitter on UART2 tx=2 rx=1 type=0x3D baud=115200 fields=2 debug=1
I lego_emit: handshake: connecting (UART2 tx=2 rx=1, 2400→115200 baud)…
I lego_emit: hub connected — handshake complete
I lego_emit: link up: mode=5 combo=1  R=2350 G=200 B=0 I=0      (debug only, every ~2 s)
```

With debug **on** you additionally get the byte-level handshake from the protocol layer
(`[LPF2] …` lines): every frame sent (`DBG_TX`), every byte received (`DBG_RX`), and the
handshake/connect events (`DBG_CONN`). The categories are `DBG_TX | DBG_RX | DBG_CONN |
DBG_INIT`; tighten the set in `lego_emit.cpp` (`lpf2_set_debug_mask(...)`) if it's too noisy.

What the trace tells you:

- **No `[LPF2]` RX bytes at all** → the hub isn't talking to us. Almost always TX/RX swapped
  or no shared GND. Try swapping `tx_gpio`/`rx_gpio` in the card and Save.
- **RX bytes but `handshake failed (no hub ACK)`** → the hub is sending but rejecting our
  descriptors; compare the `[LPF2]` TX frames against a real Color Sensor capture.
- **`hub connected` then `hub link lost`** → keepalive starvation; the task must run ≥ 20 Hz,
  so make sure nothing else long-blocking is pinned to core 1.

## Troubleshooting

- **Values look wrong**: confirm the `bits`/`scale`/`offset` on the device match the decoder,
  and that the field order is identical (the bit offsets are positional).
