# Colour calibration — teachable palette

Colour sensors (**TCS34725**, **AS7341**) classify a reading into a colour id. Beyond the
white-balance calibration, you can **teach each colour** under your own lighting/material, add
**custom colours**, and reset individual colours or the whole device. This corrects local
variations that the built-in defaults can't.

The 8 SPIKE-named default references (black/violet/blue/light blue/green/yellow/red/white) use
LEGO's own published SPIKE Color Sensor reference RGB values. The 4 extra ids that fill the hue
gaps between them (purple/cyan/orange/silver — see [below](#custom-colours-beyond-the-12-defaults))
have no published reference and remain approximate — teach them yourself for accuracy if you
use them. Note LEGO's own **element/brick colour IDs** (e.g. "LEGO:23" for a blue part) are a
*separate* numbering system from the **SPIKE `color()` sensor output ids** (0/1/3/4/6/7/9/10)
this device emits — don't confuse the two if cross-referencing LEGO's colour documentation.

A printable **[colour test sheet](assets/colour-test-sheet.pdf)** ([source](assets/colour-test-sheet.html))
lists all 12 default colours with their name/id/hex/RGB, for a quick sanity check that a sensor
and the classifier are working. It is **not** a calibration reference — printer ink/paper vary
too much for that; use it for functional testing, and [Teach](#teaching-a-colour) against real
physical samples (LEGO bricks, your actual materials) for accuracy.

### Colour correction matrix (TCS34725) — a deeper accuracy limit than white-balance

White-balance calibration only corrects each RGB channel's *overall* sensitivity (a single gain
per channel). It can't correct **spectral crosstalk** — the TCS34725's colour filters aren't a
perfect bandpass, so e.g. its blue photodiode picks up some green/yellow energy too, which can
read a saturated yellow brick as brownish even when perfectly white-balanced. Fixing that needs
a full colour-correction matrix (CCM) fitted against *several* known reference colours (not just
white) — see `TCS_CCM`/`TCS_CCM_BIAS` in
[sensor_transform.c](../firmware/components/sensor/sensor_transform.c), applied to the
white-balanced RGB right after `rgb_to_255()`, before both classification and every colour output
mode.

The current matrix was fit (weighted least-squares) from one sensor unit's readings of all 12
reference colours under one lighting setup — it brought nearest-neighbour misclassification on
that data from 3/12 (uncorrected) down to 2/12, and all 8 of LEGO's officially-published
reference colours now match correctly (the 2 remaining misses are against the *approximate*,
unpublished ids — purple and cyan — where the target itself is a guess, not solid ground truth).
It is **not guaranteed to generalise** to a different sensor unit or lighting setup — a CCM is
fit to specific measurement conditions, not a property of the chip. If accuracy still looks off
for your setup, re-fit it (see the worksheet below) or [Teach](#teaching-a-colour) against your
actual bricks/lighting for colours you need to be precise regardless.

A printable **[calibration capture worksheet](assets/colour-calibration-worksheet.pdf)**
([source](assets/colour-calibration-worksheet.html)) walks through collecting the samples needed
to fit this matrix — a setup checklist plus a recording table for white/black/red/green/blue/
yellow (the essential set) and the other 6 colours (for a fuller fit). It's a *recording sheet*,
not a colour reference — read real LEGO bricks, not the printed page (same ink/paper caveat as
the [colour test sheet](assets/colour-test-sheet.pdf) above).

## How classification works

1. **White balance first** (the existing **Calibrate** button) — point at white, capture. This
   normalises the raw reading (white-balanced RGB for TCS34725; a device-independent spectrum
   for AS7341). Always do this first, and **re-do it if you change TCS34725's poll_ms** —
   its integration time/gain are auto-derived from poll_ms (a new conversion is ready exactly
   when it's polled, instead of a fixed ~154ms integration that under-uses a slow poll interval
   or over-runs a fast one), so raw counts scale differently at a different poll rate and an old
   white reference (and any colours taught against it) will read wrong until recalibrated.
2. **No target?** If nothing reflective is close enough (or it's too dark), the sensor reports
   **id −1 ("no colour")** — the same "nothing detected" state a real LEGO colour sensor reports
   when it sees no target, rather than forcing a false match. The dashboard shows this as "none".
3. **Nearest-match** otherwise, against a palette: the **built-in defaults** (standard SPIKE
   colours) plus any **learned** entries you teach. The closest reference wins; its **report id**
   is the colour the dashboard shows and the LEGO emitter sends.

With an empty palette the result matches the previous built-in behaviour — teaching only
*overrides/extends* it.

**TCS34725 and AS7341 classify into the identical 12-colour set** (below) — same names, same
ids. AS7341 originally had two extra spectral categories (lime, pink) that duplicated green and
violet respectively; those were dropped so both sensor types share one palette and a taught
colour means the same thing regardless of which sensor captured it.

## Teaching a colour

In the **Configure** card, a colour sensor shows a **colour palette** editor listing the 12
standard colours — the 8 the SPIKE app's `color()` block names (black 0, violet 1, blue 3,
light blue 4, green 6, yellow 7, red 9, white 10) plus 4 extra ids from the classic LPF2 Color
& Distance Sensor enum that fill the hue gaps between them (purple 2, cyan 5, orange 8,
silver 11) — and any customs you add.

1. Set the sensor's **convert** to `col_lego`/`col_full` (TCS) or `as_lego`/`as_full` (AS7341).
2. **Present the colour** to the sensor.
3. Click **Teach** on that colour's row — it captures the current reading as that colour's
   reference (the row flips to **● taught**).

Repeat under your actual lighting for the colours you care about. Teaching the same colour
again overwrites its reference.

## White balance — editable calibration

**Calibrate** captures the white reference; the palette editor then **shows those calibrated
values as sliders** (clear/red/green/blue for TCS34725, the 10 spectral channels for AS7341)
so you can fine-tune the calibration directly by dragging — each slider shows its live value,
and **lowering a channel brightens that channel's output** (col_full/as_full divide the raw
reading by this reference). There is no separate gain step — the sliders edit the *real*
calibration, so RGB, reflect and classification all follow consistently and immediately.
**Save** to persist; if you move the white balance a lot, re-Teach your colours (their
references were captured against the old calibration).

## Fine-tuning a taught colour

Every palette row — **taught or still default** — has an **Edit** button that opens its
reference as **sliders**: r/g/b (white-balanced 0–255) for TCS34725, the 10 normalised
spectral channels for AS7341. On an already-taught colour this opens the captured reference; on
a still-**default** colour, opening Edit **seeds a starting reference from its nominal colour**
(e.g. "green" seeds from LEGO's green) so you can hand-tune it into a taught entry without
presenting a physical sample. Drag a channel and the row's taught swatch and the
[hue wheel](#hue-wheel) update live. **Save** to persist.

## Hue wheel

A **hue/saturation wheel** below the palette plots every colour by hue **angle** and
saturation **radius**:

- **hollow dots** — the standard SPIKE colours still on their **default** reference, at their
  nominal hue/saturation;
- **solid dots** — any **taught or custom** colour, at its actual **captured** hue/saturation
  (colour and position update live as you drag the reference sliders above).

Saturation controls the radius, not just hue the angle — **white, black and any grey sit near
the centre** regardless of hue, instead of piling onto whichever hue angle their (mathematically
undefined) hue happens to compute to. Without this, white would land on top of red at hue 0°.
Fully saturated colours sit near the rim.

Within the achromatic (near-centre) zone, colours are further **spread by brightness** rather
than all landing on one point — black, silver and white all have zero saturation and would
otherwise stack exactly on top of each other; darker colours sit toward the left of the centre
zone, brighter toward the right, so all three are visible and separately hoverable.

While **streaming/polling** is on and the sensor's mode is `col_rgb255`, `col_hue`, `col_full`
or `as_full` (any mode that carries an RGB/HSV triple), a **pulsing ring** tracks the sensor's
*current* reading live on the wheel — move a sample in front of the sensor and watch the ring
track it in real time, useful for checking a physical sample lands where you expect relative to
the taught/default dots before deciding whether to teach it. (`raw`, `col_lego`/`as_lego`, and
`as_dist` carry no RGB triple, so no live marker shows for those modes.)

Use it to check the palette is **spread out**: dots close together (default or taught) are near
each other in hue/saturation and more likely to be confused by the nearest-match classifier;
well-separated dots mean cleaner classification. **Hover a dot** to fill the detail panel below
the wheel with its name, default/taught status, report id, hue/saturation, and RGB.

## Reading filters — reduce jumpy or misclassified colours

Two independent filters below the white-balance section, both default **off** (no change to
existing behaviour) and both apply to every colour-derived value — dashboard, display, and the
LEGO emitter:

- **Smoothing** (0–95%) — an exponential moving average over the sensor's *raw* channels,
  applied before white-balance and classification. Higher values steady out sensor/electrical
  noise at the cost of reacting more slowly to a genuine colour change. This is the fix for
  readings that hover/jitter around the boundary between two colours.
- **Debounce** (0–10 reads) — requires that many **consecutive** classifications agree before
  the *reported* colour id changes; until then it keeps reporting the last stable id. This is
  the fix for a single stray misread flipping the reported colour for one tick and back —
  common when a borderline sample sits right between two references. Unlike smoothing, this
  only affects the *id* (`col_lego`/`col_full`/`as_lego`/`as_full`'s colour field); RGB/reflect
  still update every read.

Use smoothing for generally noisy readings, debounce for occasional one-tick colour flips, or
both together for a very stable but slower-reacting sensor. Because smoothing changes what the
classifier and Teach both see, **teach your colours after** settling on a smoothing value —
they're captured from the current smoothed state, so they stay consistent with live matching.

## Custom colours (beyond the 12 defaults)

**+ add custom colour** → enter a name and a **report id**:

- **0, 1, 3, 4, 6, 7, 9, 10** → confirmed named by the SPIKE app's `color()` block; the hub
  shows that colour, and your custom name discriminates it further on the dashboard.
- **2, 5, 8, 11** (purple/cyan/orange/silver) → part of the classic LPF2 enum and matched like
  any other default, but **not confirmed** to print a name in the SPIKE app's `color()` block —
  treat like a custom id there until verified on your hub.
- **>11** (fully custom): the id is still emitted — a hub program decoding
  `color_sensor.rgbi()` sees it — but the hub's native `color()` shows nothing for it.

Adding a custom captures the current reading immediately (present the colour first).

## Resets

- **Reset** (per standard colour) — removes your taught reference; that colour falls back to its
  built-in default.
- **Delete** (per custom colour) — removes the custom entry entirely.
- **Factory reset** (card header, confirm-guarded) — erases **all** configuration (sensors,
  display, LEGO emitter, and every colour palette) from NVS and restores board defaults.

## BLE commands

`{cmd:"learn_colour", sensor_id, name, out_id}` · `{cmd:"reset_colour", sensor_id, name}` ·
`{cmd:"factory_reset"}` — each returns `{ok, version}`. See [ble-protocol.md](ble-protocol.md).

The palette persists in NVS (stored as a blob, so it isn't limited by the old 4 KB string cap)
and survives reboots.
