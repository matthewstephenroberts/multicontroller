# Sensor types & convert modes

Reference for every sensor `type` the firmware supports, its `transform` (convert) modes, the
values each mode produces, and its poll-rate floor. Colour sensors and the LEGO emitter's field
packing have their own docs — see [colour-calibration.md](colour-calibration.md) and
[lego-emit.md](lego-emit.md); GPIO/ADC pins have their own doc — see [gpio-sensors.md](gpio-sensors.md).
This page covers the rest: environmental, IMU, distance, CO2, and the generic register recipe.
Mirrors `web/src/types.ts` (`DRIVER_MODES`) and `firmware/components/sensor/sensor_transform.c`.

> **Want a sensor added that isn't listed here?** Every driver on this page was written against
> the real chip/breakout, on real hardware — not from the datasheet alone. Adding a new one means
> buying the actual sensor to test against before a driver can be trusted, so the list grows as
> funding allows. See the [README's "Support MultiController development" section](../README.md#support-multicontroller-development)
> if you'd like to help fund a specific sensor, or open an issue if you already own hardware you
> can help test against.

## Named drivers

| `type` | Bus | Address | What it is |
|--------|-----|---------|------------|
| `bme280` | I2C | typically 0x76/0x77 | Bosch pressure + temperature + humidity |
| `bmp280` | I2C | 0x76 (onboard) | Bosch pressure + temperature (no humidity element) |
| `qmi8658` | I2C | 0x6A (onboard) | 6-axis IMU (accelerometer + gyroscope + die temperature) |
| `tcs34725` | I2C | 0x29 | 3-channel RGB colour sensor |
| `as7341` | I2C | 0x39 | 11-channel spectral sensor (8 visible bands + clear + NIR) |
| `vl53l1x` | I2C | 0x29 | ST time-of-flight distance sensor, full driver (short/long range) |
| `vl53l0x` | I2C | 0x29 | ST time-of-flight distance sensor, older generation (≤2m, single mode) |
| `tof10120` | I2C | 0x52 | TOF10120 distance module (onboard MCU) |
| `tofi2c` | I2C | 0x29 (adjust in recipe) | Generic TOF module family (TOF050/0200/0400) |
| `gamepad` | — | — | BLE-HID game controller state — see [hid-gamepad.md](hid-gamepad.md) |
| `gpio` | — | DA0–DA4 | Digital input pin — see [gpio-sensors.md](gpio-sensors.md) |
| `adc` | — | DA0–DA4 | Analog input pin — see [gpio-sensors.md](gpio-sensors.md) |
| `mcp3208` | SPI | `cs_index` + channel 0–7 | MCP3208 8-channel 12-bit SPI ADC — raw channel read |
| `qre1113` | SPI | `cs_index` + channel 0–7 (via mcp3208) | Analogue reflectance sensor for line following |
| `tssp_ir` | SPI | `cs_index` + channel 0–7 (via mcp3208) | TSSP4038/TSOP34840 demodulating IR receiver — object detection via 40kHz IR modulation |
| `vk36n16` | I2C | 0x65 | Vinka VK36N16I 16-key capacitive touch keypad (4x4: 0-9, *, #, A-D) |
| `generic` | I2C/SPI/UART | any | Register-read recipe — decodes any sensor not listed above |

## Environmental sensors

**bme280** — `raw`: `temp` (°C, −40..85), `pressure` (hPa, 300..1100), `humidity` (%, 0..100).

**bmp280** — `raw`: `temp` (°C, −40..85), `pressure` (hPa, 300..1100). No humidity channel — use
`bme280` if you need one and your breakout has it.

Poll floor: 20 ms for both (forced-mode conversion ~10 ms + I2C margin).

## IMU (qmi8658, onboard)

| Mode | Values | Notes |
|------|--------|-------|
| `raw` | `ax ay az` (g, ±4), `gx gy gz` (°/s, ±2000), `temp` (°C) | Raw accelerometer/gyro/die-temp |
| `imu_orient` | `roll pitch yaw` (°) | Madgwick sensor fusion. **Yaw drifts over time** — there is no magnetometer to anchor it. |
| `imu_tilt` | `pitch roll` (°) | Accelerometer-only tilt angle. No drift, but only reflects gravity direction (no yaw). |

**Calibrate** (hold the board still, click Calibrate) captures gyro bias for either orientation
mode. Poll floor: 10 ms (free-running ~235 Hz output rate; the floor is just I2C read overhead).

## Colour sensors (tcs34725, as7341)

Covered in full in [colour-calibration.md](colour-calibration.md) (teaching, white balance,
filters, hue wheel). Quick mode reference:

| Sensor | Mode | Values |
|--------|------|--------|
| tcs34725 | `raw` | `clear red green blue` (0–65535 counts) |
| tcs34725 | `col_rgb255` | `r g b` (0–255) |
| tcs34725 | `col_hue` | `hue` (0–360°), `sat val` (0–100%) |
| tcs34725 | `col_lego` | `colour` (LEGO SPIKE id, −1..10) |
| tcs34725 | `col_full` | `colour reflect r g b clear` — the first 5 are native hub passthrough (0–1024 RGB); `clear` is the 6th value, the raw uncalibrated broadband count (0–65535) passthrough itself never sends, there for LEGO fields that want it |
| as7341 | `raw` | `F1..F8 clear nir` (0–65535 each) |
| as7341 | `as_lego` | `colour` (LEGO SPIKE id, −1..10) via spectral matching |
| as7341 | `as_full` | `colour reflect r g b clear` — same shape as tcs34725's `col_full` above (`clear` = raw Clear/F-Clear channel) |
| as7341 | `as_dist` | per-colour match score (0–100) against each of the 8 standard colours |

Poll floor: 15 ms (tcs34725, integration auto-derived from poll_ms); 100 ms (as7341, two SMUX
integration passes at ~27.8 ms each plus I2C overhead).

**Black/white/silver separation (as7341)** — white calibration normalises every capture's
spectral *shape* to peak 1000, discarding absolute intensity — which is the only thing that
distinguishes the neutral colours. Teach therefore stores the RAW Clear count in the taught
ref's Clear slot (the chromatic matcher ignores that slot), and the classifier separates
taught black/white/silver by comparing live raw Clear against it. Consequences: teach and
classify under the same LED setting and distance, and keep the Clear channel unclipped when
teaching neutrals (the web Calibration summary warns if it clipped). Palettes taught by older
firmware carry no intensity information — re-Teach black/white/silver after updating.

**Saturation guard** — Calibrate and Teach refuse a capture with an F1-F8 channel clipped at
full scale (40000 counts on as7341: a 10000-count ADC ceiling at 64x analog gain, with counts
scaled ×4 to keep the historical 256x-equivalent scale; 65535 on tcs34725), and as7341 Teach
also requires a white calibration to exist first. A clipped capture flattens the spectral
shape the classifier matches on — the request fails with a message telling you to lower the
LED / add distance rather than silently storing bad data. Clear/NIR are allowed to clip (the
matcher barely weights them); the web Calibration summary warns if Clear clipped during a
teach, since that degrades black/white/silver separation.

**as7341 live spectrum view** — the Configure card's colour section shows a live 10-band bar
chart (F1 415nm … F8 680nm, Clear, NIR), scaled to the strongest band. It needs the `raw`
convert mode (the only one that streams the spectral channels) and live polling running. The
hue/sat wheel's live marker also works in `raw` mode via an RGB approximation (R≈F7+F8,
G≈F4+F5, B≈F2+F3) — but the spectrum view is the sensor's native, classifier-accurate view.

**as7341 illumination LED** — the chip drives the breakout's white LED from its LDR pin; the
sensor's **LED** slider (Configure card) sets brightness 0-100% (0 = off, the power-on
default), applied on Save at the next poll. The slider is perceptual (quadratic in current):
20% ≈ 8 mA, 50% ≈ 34 mA, 100% ≈ 124 mA — a conservative cap, well under the chip's 258 mA
maximum. The chip's dimmest ON state is a hardware floor of 4 mA (no sub-4 mA drive or PWM),
which still looks clearly bright up close — that's the LED/chip, not a settings issue. Consistent illumination makes taught colours far more
repeatable — teach and classify under the same LED setting.

## Distance sensors (vl53l1x, vl53l0x, tof10120, tofi2c)

All output a `dist` value in mm, with `dist_cm` and `dist_mm` (zero-calibrated) convert modes
built from the sensor's own configured measuring range (`dist_min_mm`/`dist_max_mm`, or the
mode's native range if left at 0):

| Type | Native range | Notes |
|------|--------------|-------|
| `vl53l1x` | 40–1300 mm (short) / 40–4000 mm (long) | `dist_mode`: 0 = short (faster, poll floor 60 ms), 1 = long (poll floor 140 ms — needs a longer integration budget) |
| `tof10120` | 0–2000 mm | Onboard-MCU module; poll floor 40 ms (~30 Hz module cycle) |
| `tofi2c` | 0–4000 mm | Generic TOF050/0200/0400-family module; default addr 0x29, register 0x00, 2 bytes — adjust in the sensor's **recipe** if your board differs |

**Calibrate** (present a zero-distance target) captures a zero offset, used by the `dist_mm`
convert mode.

> **Not supported:** VL53L5CX (8×8 multizone) — it needs an ~84 KB firmware upload to the sensor
> at boot and returns 64 zones instead of one distance value. Also blocked on funding to purchase
> the breakout — see the note at the top of this page.

## External SPI ADC (mcp3208, qre1113, tssp_ir)

An MCP3208 gives one SPI CS line 8 analogue channels — wire it to any free `cs_index` (0–4) and
pick a channel (0–7) per sensor. `qre1113` and `tssp_ir` aren't separately-addressed devices;
they're just "an MCP3208 channel plus some math", so they use the same `cs_index`/channel fields.

| Type | Mode | Values | Notes |
|------|------|--------|-------|
| `mcp3208` | `raw` | `counts` (0–4095) | Direct ADC read, for anything not covered below |
| `mcp3208` | `adc_volts` | `volts` (0–3.3V) | Assumes a 3.3V reference, same as the board rail |
| `qre1113` | `raw` | `counts` (0–4095) | Direct ADC read, no calibration/scaling — the same 12-bit MCP3208 conversion as `mcp3208`'s `raw` mode, just sourced from the QRE1113's analogue output pin |
| `qre1113` | `adc_volts` | `volts` (0–3.3V) | Same reading as `raw`, just converted to volts — no scaling to reflectance |
| `qre1113` | `line_reflect` | `reflect` (0.0 white – 1.0 black), `detected` (0/1) | Two-point calibrated (see below) — the only mode that's actually reflectance-scaled |
| `tssp_ir` | `ir_object` | `strength` (0–1), `detected` (0/1) | One-shot baseline calibrated (see below) |

`raw`/`adc_volts` on `qre1113` are diagnostic — they show exactly what the ADC pin sees (a
voltage between 0 and Vref that depends on the QRE1113's phototransistor/pull-up wiring, *not* a
0–100% reflectance), useful for checking the sensor is wired up and produces a sane, changing
value as you wave something reflective in front of it. For an actual white/black reading, use
`line_reflect` — it's the only mode with any calibration/scaling applied.

**qre1113 calibration** is two-point: place the sensor over a white surface and click
**Calibrate white**, then over black and click **Calibrate black**. `reflect` then linearly maps
white→0.0, black→1.0 (clamped); `detected` is a simple digital line-present output, 1 once
`reflect` crosses the midpoint (0.5). Before both points are calibrated, `line_reflect` assumes a
full 0–4095 ADC span instead of the raw count directly, so both `reflect` and `detected` are
still plausible pre-calibration — calibrate for an accurate 0/1 threshold on your actual surface.

**tssp_ir** (covers both TSSP4038 and TSOP34840 — both models use a 40kHz IR carrier frequency)
reads its channel as a burst of ~160 back-to-back samples per poll (a single slow sample can miss
a short IR burst) and reports the deepest dip below the sensor's idle baseline as `strength`
(0–1), with `detected` set once `strength` crosses a fixed threshold. **Calibrate** with no ball
in range captures that idle baseline; before calibrating, a full-scale (4095) baseline is assumed
so `strength`/`detected` are still meaningful, just less accurate than after calibrating. Because
of the burst read, `tssp_ir`'s poll floor is higher than a plain ADC channel — see the summary
table below.

**Grouping channels:** `qre1113`/`tssp_ir` can read more than one MCP3208 channel as a *single*
sensor — check multiple channels in the web UI's "MCP3208 channels" picker instead of adding one
sensor per channel (a line-sensor bar or an IR-receiver ring). This is one poll and one BLE
reading event for the whole group rather than one per channel, and the values/names scale with
it (`ch2_reflect`/`ch2_detected`, `ch5_strength`/`ch5_detected`, ...) — each channel's value shows
up individually for the dashboard/display's per-value show/hide and the LEGO field picker, same
as any other multi-value sensor. Calibration is **shared across the whole group**, not per
channel: white/black (or idle baseline) is captured from the average of every selected channel in
one Calibrate action — sweep the whole bar over white then black (or clear the whole IR ring of
any ball) in one motion, the same way a physical sensor bar is normally calibrated.

`line_reflect`/`ir_ball` report 2 values per channel, and every sensor is capped at 12 values
total, so a group is capped at 6 channels for those modes (`raw`/`adc_volts` are 1 value/channel,
so all 8 fit). tssp_ir's burst read splits a fixed ~320-sample budget across the group's channels
instead of bursting the full single-channel count on each one, so polling stays roughly the same
cost regardless of group size (a shorter, slightly less certain burst per channel as the group
grows, rather than the poll time multiplying by channel count).

## Touch keypad (vk36n16)

Vinka VK36N16I 16-channel capacitive touch controller (I2C, default address 0x65) — the chip
behind common 4x4 touch keypads (legend 0-9, *, #, A-D on pads TP0-TP15). No initialization is
required; the driver polls the chip's key state as one 2-byte read from register 0x00 (verified
against real hardware — the VK36N8I datasheet documents 0x02 for the 8-key sibling, but 0x00 is
what the 16-key part answers on), decoded big-endian: bit N of the 16-bit word = pad TPN. If a
board revision differs, override the register via the recipe's `reg` field (0 = use the
default); if the two 8-key banks come out swapped, flip the recipe's `byte_order`.

The driver samples the chip's *current* touch state each poll — a tap shorter than the poll
interval lands between samples and is missed entirely, so run a keypad at a fast poll rate
(20-50 ms, floor 20 ms), not the 1000 ms a new sensor defaults to.

Values: `key` (lowest-numbered touched pad 0-15, -1 when none), `bitmap` (raw 16-bit multi-touch
mask, bit N = TPN — handy for LEGO-field bit packing), `count` (touched pads). Which pad number
corresponds to which printed legend depends on the board's layout — touch each key once on the
dashboard to map them.

## Generic register recipe

For any I2C/SPI/UART sensor without a named driver, set `type: "generic"` and describe how to
decode its reading directly in the sensor's `recipe`:

```jsonc
"recipe": {
  "reg": 250,             // start register / command byte
  "length": 6,             // bytes to read
  "byte_order": "be",      // "be" | "le"
  "signed": false,
  "scale": 0.01,           // value = raw * scale + offset
  "offset": 0,
  "value_names": ["temp"]  // one entry per decoded value
}
```

The `raw` convert mode reports exactly the `value_names` you defined. A generic sensor can also
use the `dist_mm` / `dist_cm` convert modes (using its own `dist_min_mm`/`dist_max_mm`, default
0–2000 mm) if it happens to be a distance-type output. Poll floor: 50 ms (module timing varies
too much by board to set a tighter floor).

## Poll-rate floors — summary

The firmware clamps every sensor's poll interval up to a realistic minimum for its type, so the
web app's rate dropdown only offers achievable values:

| Type | Floor | Reason |
|------|-------|--------|
| qmi8658, gpio, adc, gamepad | 10 ms | Direct read / cached state, no bus conversion delay |
| tcs34725 | 15 ms | Integration time auto-derived from poll_ms |
| bme280, bmp280 | 20 ms | Forced-mode conversion (~10 ms) + I2C margin |
| vk36n16 | 20 ms | One 2-byte I2C register read; touch scan is continuous on-chip |
| mcp3208, qre1113 | 5 ms | One SPI transaction — MCP3208 conversion is a few µs plus SPI overhead |
| vl53l0x | 40 ms | Default ~33 ms ranging budget + I2C margin |
| tof10120 | 40 ms | ~30 Hz module measurement cycle |
| tssp_ir | 30 ms | 160-sample burst read holds the SPI bus for a few ms |
| generic, tofi2c | 50 ms | Module timing varies by board |
| vl53l1x (short) | 60 ms | ST short-range integration budget |
| as7341 | 100 ms | Two SMUX integration passes (~27.8 ms each) + I2C overhead |
| vl53l1x (long) | 140 ms | ST long-range integration budget |

Simulation mode (`simulate: true`) generates plausible random data on the same poll schedule
without touching the actual bus — useful for testing dashboard/display/LEGO-emitter setups
before hardware arrives, or with hardware you don't have yet.
