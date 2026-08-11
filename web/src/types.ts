// Shared types — mirror docs/ble-protocol.md.

export type BusType = "i2c" | "spi" | "uart";

// Polling cap presets for BLE notification rate limiting (Hz).
export const POLLING_CAP_PRESETS = [
  { label: "Match fastest", hz: 0 },
  { label: "50 Hz", hz: 50 },
  { label: "25 Hz", hz: 25 },
  { label: "10 Hz", hz: 10 },
  { label: "5 Hz", hz: 5 },
];

export interface Recipe {
  reg: number;
  length: number;
  byte_order: "be" | "le";
  signed: boolean;
  scale: number;
  offset: number;
  value_names: string[];
}

export interface Sensor {
  id: number;
  name: string;
  type: string; // "generic" | "bme280" | ...
  bus: BusType;
  addr?: number; // i2c 7-bit
  mux_addr?: number; // i2c mux (0 = direct)
  mux_channel?: number; // i2c mux channel (-1 = direct)
  cs_index?: number; // spi CS 0..4
  port?: number; // uart port, or (legacy single-channel) MCP3208 ADC channel 0-7
  channel_mask?: number; // "qre1113"/"tssp_ir" only: bit i = MCP3208 channel i is in this sensor's group (see newer channel_mask docs in sensor.h); 0 = legacy single-channel mode using `port`
  recipe: Recipe;
  transform: string; // derived-value mode, e.g. "raw" | "imu_orient" | "col_hue"
  calib: number[]; // per-sensor calibration captured on the device
  colours?: ColourRef[]; // learnable colour palette (colour sensors)
  colour_smooth?: number; // EMA noise filter 0 (off) - 0.95 (heavy smoothing), colour sensors
  colour_debounce?: number; // consecutive matches required before the reported id changes; 0 = off
  knob_smooth?: number; // m5_8angle only: EMA noise filter 0 (off) - 0.95 (heavy smoothing) on the reported knob values — a bare potentiometer wiper jitters a few counts at rest; same semantics as colour_smooth
  knob_invert?: boolean; // m5_8angle only: invert each knob's reported value (raw ADC_MAX - v) to match the physical unit's printed min/max labels; default true
  dist_mode?: number; // distance sensors: 0 = short range, 1 = long range (vl53l1x only; ignored elsewhere)
  led?: number; // LED brightness 0-100% (0 = off) — as7341: illumination LED (chip-controlled LDR pin); m5_8angle: per-knob value-visualisation LEDs; m5_step16: RGB ring position visualisation; ignored elsewhere
  led_sleep_s?: number; // m5_8angle/m5_step16 only: blank the visualisation LEDs after this many seconds without knob/dial movement, wake instantly on movement; -1 = never sleep (default) — same semantics as the display's sleep_after_s
  dist_min_mm?: number; // distance sensors: measuring range floor; 0/unset = mode's native minimum
  dist_max_mm?: number; // distance sensors: measuring range ceiling; 0/unset = mode's native maximum
  poll_ms: number;
  enabled: boolean;
  simulate?: boolean; // generate plausible random data instead of reading the real bus (testing without hardware)
  show: boolean; // show on the display
  page: number; // display page/group (paged mode)
  value_mask: number; // bit i = monitor value i (dashboard + display)
}

// ---- Learnable colour palette ----
// One taught/custom colour: a captured reference vector + the id it reports. out_id 0..10 are
// the official SPIKE colours; a custom out_id (>10) is still emitted but the hub shows no colour.
export interface ColourRef {
  name: string;
  out_id: number;
  learned: boolean;
  ref: number[];
}

// The last failed device action (teach/calibrate/reset), so the form can show the firmware's
// rejection message inline on the exact row/button the user clicked instead of only in the
// Activity log — a silently-refused Teach otherwise looks identical to a UI that "didn't
// update". Cleared automatically when the next action starts.
export interface SensorActionError {
  sensorId: number;
  action: "teach" | "calibrate" | "reset" | "reset_sensor";
  colour?: string; // teach/reset: the colour row the error belongs to
  message: string;
}

// The standard colour ids (the built-in classifier targets): the 8 the SPIKE app's color()
// block names, plus 4 extra ids from the classic LPF2 Color & Distance Sensor enum that fill
// the hue gaps between them (purple/cyan/orange/silver). The extras are still emitted/matched
// normally, but the SPIKE app's native color() block may not print a name for them — treat
// like a custom id there (see docs/colour-calibration.md). The palette editor lists all of
// these as always-present, teachable rows; the user can also add further custom colours.
// Gamma-encode a single 0-255 *linear* channel (proportional to reflected light intensity, e.g.
// a raw/white-balanced sensor count) into the perceptual domain a display expects. Only for
// turning a physical reading into a swatch a human looks at — never apply this before
// classification or distance matching (colour_match/as_dist_sq need the original linear
// values), and never to values emitted to the LEGO hub (a real sensor's wire values are linear
// too, so gamma-encoding them would misrepresent what's being emulated).
export function gammaEncode(linear: number): number {
  const v = Math.max(0, Math.min(255, linear)) / 255;
  return Math.round(Math.pow(v, 1 / 2.2) * 255);
}

// A live r/g/b reading (already white-balance/CCM-corrected by the firmware transform) into a
// CSS colour: scale against the mode's fixed declared max (255 for col_rgb255, 1024 for
// col_full/as_full — not the sample's own peak channel, which would wash out saturation), then
// gamma-encode for display. Shared by RgbGroup's swatch and the "colour" id dot, so the id dot
// shows the actual corrected reading when one is available instead of a fixed reference colour.
export function rgbSwatch(r: number, g: number, b: number, max: number): string {
  const scale = (x: number) => Math.max(0, Math.min(255, Math.round((x / max) * 255)));
  return `rgb(${gammaEncode(scale(r))}, ${gammaEncode(scale(g))}, ${gammaEncode(scale(b))})`;
}

// LEGO's own published SPIKE Color Sensor reference RGB, in the sensor's *linear* domain (same
// numbers as firmware's TCS_DEF in sensor_transform.c); purple/cyan/orange/silver have no
// published reference and remain approximate. The displayed swatch gamma-encodes these for
// screen display — a linear value rendered straight as a CSS colour looks duller/darker than
// the same physical colour looks to the eye.
const SPIKE_COLOURS_LINEAR: { id: number; name: string; rgb: [number, number, number] }[] = [
  { id: 0, name: "black", rgb: [0, 0, 0] },
  { id: 1, name: "violet", rgb: [144, 31, 118] },
  { id: 2, name: "purple", rgb: [140, 0, 140] },
  { id: 3, name: "blue", rgb: [30, 90, 168] },
  { id: 4, name: "light blue", rgb: [104, 195, 226] },
  { id: 5, name: "cyan", rgb: [0, 200, 200] },
  { id: 6, name: "green", rgb: [0, 133, 43] },
  { id: 7, name: "yellow", rgb: [250, 200, 10] },
  { id: 8, name: "orange", rgb: [255, 140, 0] },
  { id: 9, name: "red", rgb: [180, 0, 0] },
  { id: 10, name: "white", rgb: [244, 244, 244] },
  { id: 11, name: "silver", rgb: [192, 192, 192] },
];
function hexFromLinear(rgb: [number, number, number]): string {
  return "#" + rgb.map((v) => gammaEncode(v).toString(16).padStart(2, "0")).join("");
}
export const SPIKE_COLOURS: { id: number; name: string; swatch: string }[] =
  SPIKE_COLOURS_LINEAR.map((c) => ({ id: c.id, name: c.name, swatch: hexFromLinear(c.rgb) }));

export function isColourSensor(type: string): boolean {
  return type === "tcs34725" || type === "as7341";
}

// ---- TCS34725 starting-point calibration ----
// Captured from one real unit/lighting setup (see docs/colour-calibration.md) — a white
// reference plus all 12 colours taught against it. Seeded as a new tcs34725 sensor's *starting*
// calib/colours instead of empty, so it classifies reasonably out of the box without requiring
// Calibrate + 12x Teach first. This is still just a starting point, not a substitute for your
// own calibration: white-balance gain and CCM correction are tied to one physical unit's optics
// and one lighting setup (see TCS_CCM's own caveat in sensor_transform.c) — re-Calibrate and
// re-Teach under your actual conditions if accuracy looks off, same as you would from empty.
export const TCS34725_PRESET_CALIB: number[] = [866, 162, 288, 439];   // clear, red, green, blue
const TCS34725_PRESET_REFS: { name: string; out_id: number; ref: [number, number, number] }[] = [
  { name: "black", out_id: 0, ref: [31.65, 28.36, 10.06] },
  { name: "violet", out_id: 1, ref: [146.63, 18.86, 62.18] },
  { name: "purple", out_id: 2, ref: [143.81, 25.24, 78.64] },
  { name: "blue", out_id: 3, ref: [33.85, 137.67, 202.71] },
  { name: "light blue", out_id: 4, ref: [56.09, 202.47, 230.29] },
  { name: "cyan", out_id: 5, ref: [31.31, 194.17, 206.42] },
  { name: "green", out_id: 6, ref: [15.86, 112.56, 15.51] },
  { name: "yellow", out_id: 7, ref: [255, 117.21, 0] },
  { name: "orange", out_id: 8, ref: [255, 39.8, 0] },
  { name: "red", out_id: 9, ref: [174.77, 9.14, 28.84] },
  { name: "white", out_id: 10, ref: [250.6, 255, 242.11] },
  { name: "silver", out_id: 11, ref: [210.59, 229.86, 200.62] },
];
// Fresh array/object copies each call — callers mutate their own sensor's colours independently
// (e.g. Reset on one sensor must not affect another sensor that was also seeded from this preset).
export function defaultTcs34725Colours(): ColourRef[] {
  return TCS34725_PRESET_REFS.map((c) => ({ name: c.name, out_id: c.out_id, learned: true, ref: [...c.ref] }));
}

// Transforms with a calibration step (white reference / gyro bias / zero offset / line / IR
// baseline). Colour sensors calibrate in *any* convert mode — the white reference is captured
// from the raw channels, and the firmware keys the capture off the sensor type — so e.g. the
// as7341's "raw" spectrum-view mode still offers Calibrate (Teach requires a whitecal).
export function canCalibrate(s: Sensor): boolean {
  return isColourSensor(s.type) || /^(imu_|dist_|line_reflect|ir_ball)/.test(s.transform || "");
}

// Derive a CSS colour from a taught reference vector so the UI can show the *actual* captured
// colour. TCS34725 ref is white-balanced RGB (0-255); AS7341 ref is a 10-band spectrum — pick
// R≈F7+F8, G≈F4+F5, B≈F2+F3 and normalise (mirrors the firmware's as_full RGB approximation).
export function swatchFromRef(type: string, ref?: number[], gain = 1): string | null {
  if (!ref || ref.length < 3) return null;
  let r: number, g: number, b: number;
  if (type === "as7341") {
    r = (ref[6] ?? 0) + (ref[7] ?? 0);
    g = (ref[3] ?? 0) + (ref[4] ?? 0);
    b = (ref[1] ?? 0) + (ref[2] ?? 0);
    const mx = Math.max(r, g, b, 1);
    r = (r / mx) * 255; g = (g / mx) * 255; b = (b / mx) * 255;
  } else {
    [r, g, b] = [ref[0], ref[1], ref[2]];
  }
  const c = (x: number) => gammaEncode(x * gain);
  return `rgb(${c(r)}, ${c(g)}, ${c(b)})`;
}

// Display brightness for an as7341 taught-colour swatch. Whitecal normalises every stored
// spectrum's shape to peak 1000 — intensity is discarded, so black/white/silver would all
// render as near-white. Newer firmware stores the RAW Clear count in ref[8] (a slot the
// chromatic matcher ignores); scale swatch brightness by it relative to the palette's
// brightest taught colour. Legacy palettes (ref[8] still a whitecal value, all < ~2500)
// carry no intensity information — return 1 (unchanged) rather than dimming everything.
export function as7341SwatchGain(sensor: Sensor, ref?: number[]): number {
  if (sensor.type !== "as7341" || !ref) return 1;
  const clears = (sensor.colours ?? []).filter((c) => c.learned).map((c) => c.ref[8] ?? 0);
  const mx = Math.max(...clears, 1);
  if (mx < 2500) return 1;
  return Math.max(0.05, Math.min(1, (ref[8] ?? 0) / mx));
}

// ---- Driver / transform catalog ----
// Per driver type, the selectable transform modes and the values each produces (with units
// and default min/max used to auto-scale a LEGO field to its bit width). Mirrors the firmware
// sensor_transform.c modes. Generic sensors fall back to their recipe value names + distance.
export interface ModeValue {
  name: string;
  unit: string;
  min: number;
  max: number;
  // Continuous quantity: every value in [min,max] is meaningful, not just the integers.
  // Identity scaling ("send round(value) as-is") is only lossless for integer-valued readings —
  // applied to a continuous one it quantizes to whole units, which for a narrow range destroys
  // the reading: a calibrated 0-1 reflectance collapses to 0 or 1, and 0-3.3V to four steps.
  // The range alone can't distinguish the two cases (a 0-1 reflectance and a 0/1 detected flag
  // look identical), so it's marked here and defaultScaleOffset always fits proportionally.
  cont?: boolean;
}
export interface DriverMode {
  id: string; // matches cfg.transform
  label: string;
  values: ModeValue[];
}

const C = (name: string, unit: string, min: number, max: number): ModeValue => ({ name, unit, min, max });
// Continuous counterpart of C() — see ModeValue.cont.
const K = (name: string, unit: string, min: number, max: number): ModeValue => ({ name, unit, min, max, cont: true });

export const DRIVER_MODES: Record<string, DriverMode[]> = {
  bme280: [
    { id: "raw", label: "raw", values: [C("temp", "°C", -40, 85), C("pressure", "hPa", 300, 1100), C("humidity", "%", 0, 100)] },
  ],
  bmp280: [
    { id: "raw", label: "raw", values: [C("temp", "°C", -40, 85), C("pressure", "hPa", 300, 1100)] },
  ],
  qmi8658: [
    { id: "raw", label: "raw (accel/gyro)", values: [C("ax", "g", -4, 4), C("ay", "g", -4, 4), C("az", "g", -4, 4), C("gx", "°/s", -2000, 2000), C("gy", "°/s", -2000, 2000), C("gz", "°/s", -2000, 2000), C("temp", "°C", -40, 85)] },
    { id: "imu_orient", label: "orientation (Madgwick)", values: [C("roll", "°", -180, 180), C("pitch", "°", -90, 90), C("yaw", "°", -180, 180)] },
    { id: "imu_tilt", label: "tilt (accel only)", values: [C("pitch", "°", -90, 90), C("roll", "°", -180, 180)] },
  ],
  // Onboard IMU + magnetometer (e.g. M5Stack AtomS3R's BMI270 + BMM150) — one combined sensor,
  // not two: BMM150 is only reachable through BMI270's aux I2C port (see
  // firmware/components/imu/imu.c), so the firmware exposes both chips as a single
  // "bmi270_bmm150" driver/sensor entry (see
  // firmware/components/sensor/drivers/drv_bmi270_bmm150.c) rather than requiring the two to be
  // added and matched up separately. ax..gz is the same shape qmi8658 uses (firmware's
  // sensor_transform.c imu_orient/imu_tilt modes key off value order, not sensor type, so those
  // derived modes apply here too), at this project's configured BMI270 full-scale
  // (±4g/±1000°/s — see imu.c) rather than qmi8658's ±8g/±512°/s. mx/my/mz use BMM150's own
  // datasheet full-scale for X/Y (±1300µT; Z is wider at ±2500µT, but one shared range is
  // simpler here).
  bmi270_bmm150: [
    { id: "raw", label: "raw (accel/gyro/mag)", values: [C("ax", "g", -4, 4), C("ay", "g", -4, 4), C("az", "g", -4, 4), C("gx", "°/s", -1000, 1000), C("gy", "°/s", -1000, 1000), C("gz", "°/s", -1000, 1000), C("mx", "µT", -1300, 1300), C("my", "µT", -1300, 1300), C("mz", "µT", -1300, 1300), C("temp", "°C", -40, 85)] },
    { id: "imu_orient9", label: "orientation (9-axis, mag-corrected yaw)", values: [C("roll", "°", -180, 180), C("pitch", "°", -90, 90), C("yaw", "°", -180, 180)] },
    { id: "imu_orient", label: "orientation (6-axis, yaw drifts)", values: [C("roll", "°", -180, 180), C("pitch", "°", -90, 90), C("yaw", "°", -180, 180)] },
    { id: "imu_tilt", label: "tilt (accel only)", values: [C("pitch", "°", -90, 90), C("roll", "°", -180, 180)] },
  ],
  // TI INA226 power monitor — one type covering both places this chip shows up in this
  // project: a bare INA226 breakout wired directly to the external Port.A I2C bus, or M5Stack's
  // Atomic Motion Base v1.2's own fixed-address INA226 on its own separate bus (the bottom
  // pogo-pin header, G38/G39 — confirmed against M5Stack's own Atomic Motion Base pinout and
  // Arduino example, both of which put this base's I2C on G38(SDA)/G39(SCL), not Port.A). The
  // firmware driver (drv_ina226.c) tries Port.A first and falls back to probing the Motion
  // Base's bus automatically — the web UI never needs to know which one a given sensor is
  // actually wired to. pct is a rough single-cell Li-ion state-of-charge estimate from voltage
  // (piecewise-linear discharge-curve approximation, not coulomb counting) — good enough for
  // "getting low", not a precise gauge.
  ina226: [
    // mV, mA, mW: all measurements in milli-units for better LEGO compatibility (larger whole integers)
    { id: "raw", label: "voltage/current/power/battery %", values: [C("voltage_mV", "mV", 3000, 4200), C("current_mA", "mA", -8000, 8000), C("power_mW", "mW", 0, 30000), C("pct", "%", 0, 100)] },
  ],
  tcs34725: [
    { id: "raw", label: "raw counts", values: [C("clear", "", 0, 65535), C("red", "", 0, 65535), C("green", "", 0, 65535), C("blue", "", 0, 65535)] },
    { id: "col_rgb255", label: "RGB 0-255", values: [C("r", "", 0, 255), C("g", "", 0, 255), C("b", "", 0, 255)] },
    { id: "col_hue", label: "HSV", values: [C("hue", "°", 0, 360), C("sat", "%", 0, 100), C("val", "%", 0, 100)] },
    { id: "col_lego", label: "LEGO colour id (SPIKE)", values: [C("colour", "", -1, 11)] },
    { id: "col_full", label: "colour + reflect + RGB + raw clear (passthrough)", values: [C("colour", "", -1, 11), C("reflect", "%", 0, 100), C("r", "", 0, 1024), C("g", "", 0, 1024), C("b", "", 0, 1024), C("clear", "", 0, 65535)] },
  ],
  as7341: [
    // Full scale 40000: the 10000-count ADC ceiling × the driver's 4x count scaling (see
    // drv_as7341.c AS7341_COUNT_SCALE) — not the 16-bit register maximum.
    { id: "raw", label: "raw spectral (F1-F8,Clear,NIR)", values: [C("F1", "", 0, 40000), C("F2", "", 0, 40000), C("F3", "", 0, 40000), C("F4", "", 0, 40000), C("F5", "", 0, 40000), C("F6", "", 0, 40000), C("F7", "", 0, 40000), C("F8", "", 0, 40000), C("clear", "", 0, 40000), C("nir", "", 0, 40000)] },
    { id: "as_lego", label: "LEGO colour (spectral, SPIKE)", values: [C("colour", "", -1, 11)] },
    { id: "as_full", label: "colour + reflect + RGB + raw clear (passthrough)", values: [C("colour", "", -1, 11), C("reflect", "%", 0, 100), C("r", "", 0, 1024), C("g", "", 0, 1024), C("b", "", 0, 1024), C("clear", "", 0, 40000)] },
    { id: "as_dist", label: "colour match scores", values: [C("black", "", 0, 100), C("white", "", 0, 100), C("red", "", 0, 100), C("yellow", "", 0, 100), C("green", "", 0, 100), C("lblue", "", 0, 100), C("blue", "", 0, 100), C("violet", "", 0, 100)] },
  ],
  // vl53l1x / tof10120 / tofi2c (distance sensors) are built dynamically by distModes()
  // below, from each sensor's own dist_mode/dist_min_mm/dist_max_mm — not listed here.
  gamepad: [
    { id: "raw", label: "buttons + sticks", values: [
      C("buttons", "", 0, 65535), C("lx", "", -32768, 32767), C("ly", "", -32768, 32767),
      C("rx", "", -32768, 32767), C("ry", "", -32768, 32767), C("lt", "", 0, 1023),
      C("rt", "", 0, 1023), C("dpad", "", 0, 8),
    ] },
    // Sticks quantized to -7..+7 (signed 4-bit, with a 10% centre deadzone so resting = 0)
    // and triggers to 0..15 — digital-friendly values that pack into tiny LEGO fields with
    // identity scale, like dpad but with usable resolution.
    // ldir/rdir: each stick's x+y folded into one 8-way compass code with the dpad's encoding
    // (0=centred, 1=up … clockwise … 8=up-left) — a whole stick in one unsigned 4-bit field.
    { id: "pad_digital", label: "digital (packs small: sticks ±7, triggers 0-15, stick compass)", values: [
      C("buttons", "", 0, 65535), C("lx7", "", -7, 7), C("ly7", "", -7, 7),
      C("rx7", "", -7, 7), C("ry7", "", -7, 7), C("lt15", "", 0, 15),
      C("rt15", "", 0, 15), C("dpad", "", 0, 8), C("ldir", "", 0, 8), C("rdir", "", 0, 8),
    ] },
  ],
  gpio: [
    { id: "raw", label: "digital (0/1)", values: [C("state", "", 0, 1)] },
  ],
  adc: [
    { id: "raw", label: "ADC counts", values: [C("counts", "", 0, 4095)] },
    { id: "adc_volts", label: "volts", values: [K("volts", "V", 0, 3.3)] },
  ],
  mcp3208: [
    { id: "raw", label: "ADC counts", values: [C("counts", "", 0, 4095)] },
    { id: "adc_volts", label: "volts", values: [K("volts", "V", 0, 3.3)] },
  ],
  // qre1113/tssp_ir single-channel (channel_mask unset) fall back to these; a grouped sensor
  // (channel_mask set) gets its modes built dynamically by groupedModes() below, one name per
  // selected channel instead of one fixed set — see sensorModes().
  qre1113: [
    { id: "raw", label: "raw ADC counts (no scaling)", values: [C("counts", "", 0, 4095)] },
    { id: "adc_volts", label: "volts (no scaling, just counts→V)", values: [K("volts", "V", 0, 3.3)] },
    { id: "line_reflect", label: "reflectance (white-black) + detected — calibrated", values: [K("reflect", "", 0, 1), C("detected", "", 0, 1)] },
  ],
  tssp_ir: [
    { id: "ir_ball", label: "IR object + strength", values: [K("strength", "", 0, 1), C("detected", "", 0, 1)] },
  ],
  vk36n16: [
    { id: "raw", label: "key + bitmap + count", values: [C("key", "", -1, 15), C("bitmap", "", 0, 65535), C("count", "", 0, 16)] },
  ],
  m5_8angle: [
    { id: "raw", label: "8 knobs (12-bit) + switch", values: [
      C("k0", "", 0, 4095), C("k1", "", 0, 4095), C("k2", "", 0, 4095), C("k3", "", 0, 4095),
      C("k4", "", 0, 4095), C("k5", "", 0, 4095), C("k6", "", 0, 4095), C("k7", "", 0, 4095),
      C("switch", "", 0, 1),
    ] },
    { id: "knob_digital", label: "8 knobs (jitter-filtered, full range) + switch", values: [
      C("k0", "", 0, 4095), C("k1", "", 0, 4095), C("k2", "", 0, 4095), C("k3", "", 0, 4095),
      C("k4", "", 0, 4095), C("k5", "", 0, 4095), C("k6", "", 0, 4095), C("k7", "", 0, 4095),
      C("switch", "", 0, 1),
    ] },
  ],
  m5_step16: [
    { id: "raw", label: "step position (0-15)", values: [C("position", "", 0, 15)] },
  ],
};

// A grouped qre1113/tssp_ir sensor reports up to 2 values per selected channel, and every sensor
// is capped at MC_MAX_VALUES (16) total — mirrors the same clamp in config_store.c's
// parse_sensor(), which drops channels beyond this from the saved channel_mask. 16 / 2 = all 8
// mcp3208 channels fit.
export const MAX_GROUP_CHANNELS = 8;

function channelList(mask: number): number[] {
  const chans: number[] = [];
  for (let ch = 0; ch < 8; ch++) if (mask & (1 << ch)) chans.push(ch);
  return chans;
}

// Modes for a grouped qre1113/tssp_ir sensor (channel_mask set): the same per-channel value
// shapes as the single-channel modes above, repeated once per selected channel and named
// "ch2_reflect"/"ch2_detected" etc. — mirrors sensor_transform_names()'s grouped naming in
// firmware/components/sensor/sensor_transform.c so the dashboard/LEGO field picker line up with
// what the device actually streams.
function groupedModes(s: Sensor): DriverMode[] {
  const chans = channelList(s.channel_mask ?? 0);
  const raw: DriverMode = { id: "raw", label: "raw ADC counts (no scaling)", values: chans.map((c) => C(`ch${c}`, "", 0, 4095)) };
  if (s.type === "qre1113") {
    const volts: DriverMode = { id: "adc_volts", label: "volts (no scaling, just counts→V)", values: chans.map((c) => K(`ch${c}_volts`, "V", 0, 3.3)) };
    const reflect: DriverMode = {
      id: "line_reflect", label: "reflectance (white-black) + detected — calibrated",
      values: chans.flatMap((c) => [K(`ch${c}_reflect`, "", 0, 1), C(`ch${c}_detected`, "", 0, 1)]),
    };
    return [raw, volts, reflect];
  }
  const irObject: DriverMode = {
    id: "ir_ball", label: "IR object + strength",
    values: chans.flatMap((c) => [K(`ch${c}_strength`, "", 0, 1), C(`ch${c}_detected`, "", 0, 1)]),
  };
  return [irObject];
}

// The board's five "DIGITAL ANALOGUE" pins (mirrors BOARD_DA_GPIOS in firmware/main/board_config.h).
// gpio/adc sensors pick one of these; the chosen GPIO is stored in Sensor.port.
export const BOARD_DA_PINS: { label: string; gpio: number }[] = [
  { label: "DA0 (GPIO18)", gpio: 18 },
  { label: "DA1 (GPIO17)", gpio: 17 },
  { label: "DA2 (GPIO16)", gpio: 16 },
  { label: "DA3 (GPIO15)", gpio: 15 },
  { label: "DA4 (GPIO14)", gpio: 14 },
];

// Gamepad button bitmask (mirrors HID_BTN_* in firmware/components/hid_host/hid_host.h),
// used by the Gamepad card's live visualiser.
export const GAMEPAD_BUTTONS: { name: string; bit: number }[] = [
  { name: "A", bit: 0 }, { name: "B", bit: 1 }, { name: "X", bit: 2 }, { name: "Y", bit: 3 },
  { name: "LB", bit: 4 }, { name: "RB", bit: 5 }, { name: "View", bit: 6 }, { name: "Menu", bit: 7 },
  { name: "LS", bit: 8 }, { name: "RS", bit: 9 }, { name: "Xbox", bit: 10 }, { name: "Share", bit: 11 },
  { name: "↑", bit: 12 }, { name: "↓", bit: 13 }, { name: "←", bit: 14 }, { name: "→", bit: 15 },
];

// Full snapshot pushed to the board via {cmd:"hid_set_state"} while the virtual controller
// (hid_virtual) is enabled — same field layout as the `gamepad` sensor's reading (see
// docs/hid-gamepad.md), sent as a full state each time rather than a delta.
export interface VirtualGamepadState {
  buttons: number;
  lx: number; ly: number; rx: number; ry: number;
  lt: number; rt: number;
  dpad: number;
}

// ---- Distance sensor measuring range ----
// Named distance drivers: native mm range per dist_mode (0=short,1=long). Sensors that don't
// support a mode (module boards) just repeat the same range for both indices.
const DIST_NATIVE_RANGE_MM: Record<string, [{ min: number; max: number }, { min: number; max: number }]> = {
  vl53l1x:  [{ min: 40, max: 1300 }, { min: 40, max: 4000 }],   // ST short / long distance mode
  vl53l0x:  [{ min: 30, max: 2000 }, { min: 30, max: 2000 }],   // older ST chip, single mode
  tof10120: [{ min: 0, max: 2000 }, { min: 0, max: 2000 }],
  tofi2c:   [{ min: 0, max: 4000 }, { min: 0, max: 4000 }],
};
export const DISTANCE_TYPES = Object.keys(DIST_NATIVE_RANGE_MM);
export function isDistanceSensor(s: Sensor): boolean {
  return DISTANCE_TYPES.includes(s.type);
}

// Effective measuring range for a distance sensor: the user's configured dist_min_mm/
// dist_max_mm if set, else the mode's native range. This is what the LEGO field editor reads
// (via sensorValueMeta) to auto-derive its scale/offset — set the range here once and any LEGO
// field sourced from this sensor scales into it automatically.
export function distanceRangeMm(s: Sensor): { min: number; max: number } {
  const native = DIST_NATIVE_RANGE_MM[s.type]?.[s.dist_mode ? 1 : 0] ?? { min: 0, max: 2000 };
  const min = s.dist_min_mm ?? native.min;
  const max = s.dist_max_mm || native.max;
  return { min, max };
}

function distModes(s: Sensor): DriverMode[] {
  const { min, max } = distanceRangeMm(s);
  return [
    { id: "raw", label: "distance (mm)", values: [C("dist", "mm", min, max)] },
    { id: "dist_cm", label: "distance (cm)", values: [C("dist", "cm", min / 10, max / 10)] },
    { id: "dist_mm", label: "mm (zero-cal)", values: [C("dist", "mm", 0, max - min)] },
  ];
}

// Distance transforms available to any (generic) sensor, using its own configured range.
function genericDistModes(s: Sensor): DriverMode[] {
  const min = s.dist_min_mm ?? 0;
  const max = s.dist_max_mm || 2000;
  return [
    { id: "dist_mm", label: "distance (mm)", values: [C("dist", "mm", min, max)] },
    { id: "dist_cm", label: "distance (cm)", values: [C("dist", "cm", min / 10, max / 10)] },
  ];
}

// Transform modes offered for a sensor's type (for the mode dropdown).
export function sensorModes(s: Sensor): DriverMode[] {
  if (isDistanceSensor(s)) return distModes(s);
  if ((s.type === "qre1113" || s.type === "tssp_ir") && (s.channel_mask ?? 0) !== 0) return groupedModes(s);
  const named = DRIVER_MODES[s.type];
  if (named) return named;
  // generic / unknown: raw (recipe names) + distance presets
  const raw: DriverMode = {
    id: "raw",
    label: "raw",
    values: s.recipe.value_names.map((n) => C(n, "", 0, 1000)),
  };
  return [raw, ...genericDistModes(s)];
}

// The active transform mode's value metadata for a sensor.
export function modeValues(s: Sensor): ModeValue[] {
  const tf = s.transform || "raw";
  const m = sensorModes(s).find((x) => x.id === tf);
  if (m) return m.values;
  // transform set but not in this type's list (e.g. a stale mode) — fall back to raw
  return sensorModes(s)[0]?.values ?? [];
}

export function sensorValues(s: Sensor): string[] {
  return modeValues(s).map((v) => v.name);
}

export function sensorValueMeta(s: Sensor, idx: number): ModeValue | undefined {
  return modeValues(s)[idx];
}

export function valueSelected(mask: number, i: number): boolean {
  return ((mask ?? 0xffff) & (1 << i)) !== 0;
}

// ---- Polling rate ----
// Mirrors sensor_poll_floor_ms() in firmware/components/sensor/sensor.c: each driver's own
// realistic floor (hardware integration/conversion time + typical I2C overhead for that
// driver), not one arbitrary default — so the dropdown only offers rates the hardware can
// actually hit. Keep this in sync with that function if either changes.
export function pollMsFloor(s: Sensor): number {
  switch (s.type) {
    case "vl53l1x": return s.dist_mode ? 140 : 60;   // ST long/short distance-mode budget + margin
    case "as7341":  return 35;                        // overlapped read: one ~27.8ms on-chip integration per visit; fresh frame every 2 visits
    case "tcs34725": return 15;                        // integration auto-derived from poll_ms down to ~2.4ms + margin
    case "bme280":
    case "bmp280":  return 20;                         // forced-mode conversion (~10ms) + I2C margin
    case "qmi8658": return 10;                          // free-running ~235Hz ODR; floor is I2C read overhead
    case "bmi270_bmm150": return 20;                    // combined accel+gyro+mag read; floor set by the mag's
                                                         // forced-mode conversion, slower than the ~100Hz accel/gyro ODR
    case "ina226": return 20;                           // ~2.2ms conversion + I2C overhead for 3 register reads;
                                                         // battery voltage doesn't need to be read fast
    case "vl53l0x": return 40;                          // default ~33ms ranging budget + I2C margin
    case "tof10120": return 40;                         // typical module measurement cycle (~30Hz max)
    case "gpio":
    case "adc":
    case "gamepad": return 10;                          // direct MCU read / cached HID state, no bus conversion
    case "vk36n16": return 20;                          // one 2-byte I2C register read; touch scan is continuous on-chip
    case "m5_8angle": return 100;                        // 8 separate per-channel I2C reads per poll — see sensor.c
    case "m5_step16": return 50;                         // cheap STM32-based I2C unit, not a dedicated ASIC — can lock up under rapid polling; conservative unmeasured floor, see sensor.c
    case "mcp3208":
    case "qre1113": return 5;                           // one SPI transaction, MCP3208 conversion is a few µs
    case "tssp_ir": return 30;                          // 160-sample burst read holds the SPI bus for a few ms
    default: return 50;                                 // generic recipe + tofi2c (module timing varies by board)
  }
}

export function hzLabel(ms: number): string {
  const hz = 1000 / ms;
  return `${ms}ms (${hz >= 10 ? Math.round(hz) : Math.round(hz * 10) / 10}Hz)`;
}

// Common poll-rate presets, ms. Filtered per-sensor down to what the firmware will actually
// honour (below the floor, the firmware clamps it anyway) — see pollMsFloor().
const POLL_MS_PRESETS = [2000, 1000, 500, 200, 100, 50, 33, 20, 10];
export function pollMsOptions(s: Sensor): number[] {
  const floor = pollMsFloor(s);
  const opts = POLL_MS_PRESETS.filter((ms) => ms >= floor);
  if (!opts.includes(floor)) opts.push(floor);
  return opts.sort((a, b) => b - a);
}

// ---- LEGO field ↔ min/max auto-scale ----
// A field encodes value = raw*scale + offset with raw an integer in the bit-width range.
// These convert between (scale, offset) and the (min, max) value range, so the UI can present
// intuitive min/max while the firmware keeps using scale/offset.
export function scaleFromRange(min: number, max: number, bits: number, signed: boolean): { scale: number; offset: number } {
  const levels = (1 << bits) - 1 || 1;
  const scale = (max - min) / levels;
  const offset = signed ? min + scale * (1 << (bits - 1)) : min;
  return { scale, offset };
}
export function rangeFromScale(scale: number, offset: number, bits: number, signed: boolean): { min: number; max: number } {
  const levels = (1 << bits) - 1;
  if (signed) {
    const half = 1 << (bits - 1);
    return { min: offset - scale * half, max: offset + scale * (half - 1) };
  }
  return { min: offset, max: offset + scale * levels };
}

export type DisplayController = "st7789" | "ili9341" | "gc9107" | "st7735s" | "ssd1306";
// "summary"/"paged" are the text modes (exact values, one line per sensor — good for reading
// numbers precisely). "tiles" is the visual mode: every shown sensor as a small grid tile,
// rendered by shape rather than digits (a colour sensor's tile IS the colour, a distance
// sensor's tile is a fill bar, a boolean-shaped value is a filled-vs-outline swatch) — good
// for glancing at several sensors at once. All three share the same device-name header and
// per-sensor show/page grouping (BOOT cycles pages in "paged" and "tiles").
export type DisplayMode = "summary" | "paged" | "tiles";

export interface DisplayConfig {
  enabled: boolean;
  controller: DisplayController;
  bus: "spi" | "i2c";
  cs: number; // SPI CS GPIO (-1 none)
  dc: number;
  rst: number;
  bl: number;
  addr: number; // I2C address (SSD1306)
  width: number;
  height: number;
  x_gap: number;
  y_gap: number;
  mirror_x: boolean;
  mirror_y: boolean;
  invert: boolean;
  mode: DisplayMode;
  // "tiles" mode only: 0/undefined = auto (fits as many as reasonably legible by aspect
  // ratio), or a fixed tiles-per-screen cap with a matching clean layout — extra shown
  // sensors spill onto further BOOT-cycled screens instead of shrinking to fit.
  tiles_per_page?: 0 | 1 | 2 | 4 | 8;
  // "tiles" mode only: group tiles into per-category blocks (Distance/Colour/Motion/
  // Environment/Line-IR/Input/Other — same categories/mapping as the Dashboard's own "group by
  // type" toggle) instead of one flat grid. Ignored alongside a fixed tiles_per_page count.
  group_tiles?: boolean;
  // Auto-sleep: backlight off after this many idle seconds (no BOOT-button press); -1 = always
  // on (disabled). A press instantly wakes it.
  sleep_after_s?: number;
  // Brief device-name splash shown once at boot before the first status/tile screen. Off skips
  // straight to it for the fastest possible startup.
  show_boot_logo?: boolean;
  // Backlight brightness 0-100%. Real dimming only where the backlight is PWM-capable (the
  // AtomS3R's LP5562-driven panel); plain-GPIO backlights treat it as on/off (>0 = on).
  brightness?: number;
}

export interface Discovered {
  bus: BusType;
  kind?: "mux" | "sensor" | "display"; // mux/display = infrastructure, not a sensor
  builtin?: boolean;
  controller?: string;
  addr?: number;
  mux_addr?: number;
  channel?: number;
  channels?: number; // mux: number of downstream channels
  cs_index?: number;
  port?: number;
  guess: string;
  // Onboard-display entries (kind:"display", builtin:true) only — the actual wired SPI pins
  // for *this* board's panel (board_config.h's BOARD_TFT_CS/DC/RST/BL_GPIO), straight from the
  // firmware. Lets "Use as display" fully reset a stale saved config, not just controller+size.
  cs?: number;
  dc?: number;
  rst?: number;
  bl?: number;
}

export function defaultDisplay(): DisplayConfig {
  return {
    enabled: false,
    controller: "st7789",
    bus: "spi",
    cs: 7,
    dc: 39,
    rst: 40,
    bl: 45,
    addr: 0x3c,
    width: 240,
    height: 135,
    x_gap: 40,
    y_gap: 53,
    mirror_x: false,
    mirror_y: true,
    invert: true,
    mode: "summary",
    tiles_per_page: 0,
    group_tiles: false,
    sleep_after_s: -1,
    show_boot_logo: true,
    brightness: 100,
  };
}

export interface Reading {
  sensor: number; // sensor id (matches the event's "sensor" field)
  ts: number;
  values: number[];
  status: string;
}

export interface ReadingHistoryEntry {
  ts: number;
  values: number[];
  status: string;
}

export interface SensorTimeSeries {
  id: number;
  entries: ReadingHistoryEntry[];
  lastUpdateMs: number;
  frequencyHz: number;
}

// ---- LEGO color-sensor emitter ----
// The board can emulate a LEGO Powered Up Color Sensor and pack selected sensor values
// into its 4×uint16 (64-bit) RGBI payload. Each field maps one decoded sensor value into
// a run of bits, packed LSB-first; the hub decodes via color.rgbi(). See docs/lego-emit.md.
// Field widths for RGBI bit-packing. 1/2-bit widths let genuinely tiny values (a detected
// flag, a 0-8 dpad code in 4) pack tight instead of burning a whole nibble/byte each —
// the firmware packer accepts any 1..16-bit width (lego_emit.cpp).
export type LegoBits = 1 | 2 | 4 | 8 | 16;

// Which slot a field writes to. LEGO_TARGET_RGBI (default) bit-packs into the 64-bit RGBI word
// like before; LEGO_TARGET_COLOR/LEGO_TARGET_REFLT instead drive the emulated sensor's COLOR
// (mode 0) / REFLT (mode 1) byte directly — those are separate values the hub can request
// without ever touching the RGBI word (mode 5), so a bit-packed field is invisible to
// color()/reflection() (or their word-block equivalents) no matter where it sits in the word.
// Only one field of each of COLOR/REFLT is meaningful; the last one configured wins. Mirrors
// LEGO_TARGET_* in sensor.h.
export const LEGO_TARGET_RGBI = 0;
export const LEGO_TARGET_COLOR = 1;
export const LEGO_TARGET_REFLT = 2;

// Colour ids the hub's color() actually supports — anything else on the COLOR byte gets
// coerced by hub firmware (e.g. 5 reads back as 6). 255 = "no colour" (color() -> none).
export const HUB_SUPPORTED_COLOUR_IDS = [0, 1, 3, 4, 6, 7, 9, 10] as const;
export const LEGO_COLOUR_NONE = 255;

export interface LegoField {
  sensor_id: number; // source sensor id
  value_index: number; // which decoded value of that sensor
  bits: LegoBits; // field width (LEGO_TARGET_RGBI only — COLOR/REFLT are always one byte)
  signed: boolean; // encode as two's-complement (LEGO_TARGET_RGBI only)
  scale: number; // raw = round((value - offset) / scale)
  offset: number;
  target?: number; // LEGO_TARGET_* — undefined/0 = RGBI (legacy default)
  // Optional second-stage scaling: final = raw * output_scale + output_offset
  // Use this to map the packed field range (0..2^bits-1) to a final LEGO output range
  // e.g., 0-15 field → 48-108 piano notes: output_scale=4, output_offset=48
  output_scale?: number;
  output_offset?: number;
  // COLOR target only: translate the scaled code (0..15) through this lookup of colour ids
  // (255 = none), so e.g. dpad codes deliberately map onto hub-supported colours instead of
  // being coerced. Present = enabled; the firmware sends colour_map[code].
  colour_map?: number[];
}

// Emulated device profile — selects which LEGO device the emitter pretends to be. The hub
// binds its API / routes its blocks by the announced LPF2 type byte, so receiving writes
// (the 3×3 matrix) requires the matrix profile. Mirrors LEGO_PROFILE_* in sensor.h.
export const LEGO_PROFILE_COLOR = 0;
export const LEGO_PROFILE_MATRIX = 1;

export interface LegoConfig {
  enabled: boolean;
  profile: number; // LEGO_PROFILE_COLOR | LEGO_PROFILE_MATRIX
  debug: boolean; // verbose: full LPF2 handshake/TX/RX byte trace
  events: boolean; // simple: just hub mode SELECT / combo / WRITE events
  colour_source: number; // sensor id (in col_full/as_full mode) driving COLOR/REFLT/RGB; 0 = bit-packing
  sensor_type: number; // LPF2 type byte (0x3D = Color Sensor)
  uart_port: number; // ESP-IDF UART number
  tx_gpio: number;
  rx_gpio: number;
  baud: number; // operational baud after handshake
  fields: LegoField[];
}

export const LEGO_TOTAL_BITS = 64;
export const LEGO_MAX_FIELDS = 16;

export function defaultLego(): LegoConfig {
  return {
    enabled: false,
    profile: LEGO_PROFILE_COLOR,
    debug: false,
    events: false,
    colour_source: 0,
    sensor_type: 0x3d,
    uart_port: 2,
    tx_gpio: 2, // matches board_config.h BOARD_LEGO_TX_GPIO (TX -> hub RX)
    rx_gpio: 1, // matches board_config.h BOARD_LEGO_RX_GPIO (RX <- hub TX)
    baud: 115200,
    fields: [],
  };
}

// Only RGBI-target fields consume the 64-bit word's budget — COLOR/REFLT targets are separate
// single-byte slots outside it (see LEGO_TARGET_* above).
export function legoBitsUsed(fields: LegoField[]): number {
  return fields.reduce((sum, f) => sum + ((f.target ?? LEGO_TARGET_RGBI) === LEGO_TARGET_RGBI ? f.bits || 0 : 0), 0);
}

export interface LegoPackedRow {
  sensorId: number;
  valueIndex: number;
  value: number; // live source value (0 if no reading yet)
  raw: number; // encoded integer placed in the bitfield (or the COLOR/REFLT byte)
  bits: LegoBits;
  signed: boolean;
  target: number; // LEGO_TARGET_*
}
export interface LegoPacked {
  channels: [number, number, number, number]; // R, G, B, I (what the hub receives)
  color: number | null; // COLOR byte (mode 0), or null if no field targets it
  reflect: number | null; // REFLT byte (mode 1), or null if no field targets it
  rows: LegoPackedRow[];
}

// Reproduce the firmware bit-packer (lego_emit.cpp current_rgbi/current_color_reflt) so the UI
// can show exactly what is being sent: raw = round((value - offset) / scale), then a code→value
// map lookup if one's configured (colour_map[raw], or LEGO_COLOUR_NONE/255 for a code past the
// table — same for every target, see lego_emit.cpp), clamped to the field width (or to a byte
// for COLOR/REFLT), RGBI-target fields packed LSB-first into a 64-bit word then split into the 4
// RGBI channels; COLOR/REFLT-target fields instead go straight to their own byte, bypassing the
// word entirely (the last field targeting each one wins).
export function packLego(fields: LegoField[], readings: Record<number, Reading>): LegoPacked {
  let word = 0n;
  let bitoff = 0n;
  let color: number | null = null;
  let reflect: number | null = null;
  const rows: LegoPackedRow[] = fields.map((f) => {
    const target = f.target ?? LEGO_TARGET_RGBI;
    const value = readings[f.sensor_id]?.values?.[f.value_index] ?? 0;
    const scale = f.scale === 0 ? 1 : f.scale;
    let raw = Math.round((value - f.offset) / scale);

    if (target !== LEGO_TARGET_RGBI) {
      // A negative value on COLOR is the classifier's "no colour" -1, sent as 0xFF (the hub reads
      // it back as -1) rather than clamped to 0/black — see current_color_reflt(). REFLT is a
      // plain 0-100 percentage with no sentinel, so it still clamps.
      if (raw < 0 && target === LEGO_TARGET_COLOR) {
        color = LEGO_COLOUR_NONE;
        return { sensorId: f.sensor_id, valueIndex: f.value_index, value, raw: LEGO_COLOUR_NONE, bits: f.bits, signed: f.signed, target };
      }
      // current_color_reflt() clamps to a byte *before* the map lookup — a code that scaled
      // outside 0-255 is still a valid array index into colour_map once clamped.
      raw = Math.max(0, Math.min(255, raw));
      if (f.colour_map) raw = (raw >= 0 && raw < f.colour_map.length) ? f.colour_map[raw] : LEGO_COLOUR_NONE;
      if (target === LEGO_TARGET_COLOR) color = raw; else reflect = raw;
      return { sensorId: f.sensor_id, valueIndex: f.value_index, value, raw, bits: f.bits, signed: f.signed, target };
    }

    // current_rgbi() looks the map up on the *unclamped* raw and treats any out-of-table index —
    // negative as well as past the end — as "no colour". Clamping a negative index to 0 here
    // instead (as this once did) reported colour_map[0] for values the device actually sends 255
    // for, and did so precisely for the signed stick codes the map feature exists to serve.
    if (f.colour_map) {
      raw = (raw >= 0 && raw < f.colour_map.length) ? f.colour_map[raw] : LEGO_COLOUR_NONE;
    }

    // Optional second-stage output scaling: map field raw value to custom LEGO output range
    // e.g., 0-15 (4-bit) → 48-108 (piano scale): output_scale=4, output_offset=48
    if (f.output_scale && f.output_scale !== 0) {
      raw = Math.round(raw * f.output_scale + (f.output_offset ?? 0));
    }

    // Budget stop, matching current_rgbi()'s `if (bitoff + bits > MC_LEGO_TOTAL_BITS) break;` —
    // a field that doesn't fit the 64-bit word is dropped whole by the device, not truncated.
    // Without this the preview kept shifting and silently lost the high bits instead, so an
    // over-budget config (which the editor flags but still lets you save) previewed differently
    // from what the hub would actually receive.
    if (Number(bitoff) + f.bits > LEGO_TOTAL_BITS) {
      return { sensorId: f.sensor_id, valueIndex: f.value_index, value, raw: 0, bits: f.bits, signed: f.signed, target };
    }

    const mask = (1 << f.bits) - 1; // bits ≤ 16, safe in a JS number
    if (f.signed) {
      raw = Math.max(-(1 << (f.bits - 1)), Math.min((1 << (f.bits - 1)) - 1, raw));
    } else {
      raw = Math.max(0, Math.min(mask, raw));
    }
    word |= BigInt(raw & mask) << bitoff; // raw & mask = unsigned two's-complement bits
    bitoff += BigInt(f.bits);
    return { sensorId: f.sensor_id, valueIndex: f.value_index, value, raw, bits: f.bits, signed: f.signed, target };
  });
  const channels: [number, number, number, number] = [
    Number(word & 0xffffn),
    Number((word >> 16n) & 0xffffn),
    Number((word >> 32n) & 0xffffn),
    Number((word >> 48n) & 0xffffn),
  ];
  return { channels, color, reflect, rows };
}

export const NAMED_TYPES = ["generic", "bmp280", "bme280", "qmi8658", "bmi270_bmm150", "ina226", "tcs34725", "as7341", "vl53l1x", "vl53l0x", "tof10120", "tofi2c", "gamepad", "gpio", "adc", "mcp3208", "qre1113", "tssp_ir", "vk36n16", "m5_8angle", "m5_step16"] as const;

// Named driver types per bus, for buses where the scan can't identify a device by probing (SPI
// has no addressing/ID register scheme, UART sensors don't self-announce) — the scanner always
// reports these as "unknown", so the user picks the type from this list instead of a fixed guess.
// generic covers anything not in the list (custom register recipe).
export const BUS_DRIVER_TYPES: Record<"spi" | "uart", string[]> = {
  spi: ["generic", "mcp3208", "qre1113", "tssp_ir"],
  uart: ["generic"],
};

// One-line description per sensor type, shown next to the type selector so it's clear what a
// type actually is/does without having to know the part number already.
export const TYPE_DESCRIPTIONS: Record<string, string> = {
  generic: "Custom register-read recipe — decode any I2C/SPI/UART sensor not listed below by its raw register layout.",
  bmp280: "Bosch barometric pressure + temperature sensor (I2C).",
  bme280: "Bosch pressure + temperature + humidity sensor (I2C) — bmp280 plus a humidity element.",
  qmi8658: "QST 6-axis IMU (3-axis accel + 3-axis gyro, I2C) — orientation/tilt/motion sensing.",
  bmi270_bmm150: "Bosch 6-axis IMU (accel+gyro) + BMM150 3-axis magnetometer, combined — built into the M5Stack AtomS3R's onboard sensor stack, not a user-wired I2C device. The magnetometer enables mag-corrected yaw (orientation 9-axis mode) instead of the drift-prone 6-axis fallback.",
  ina226: "TI power monitor (I2C, address 0x40) — voltage (mV), current (mA), power (mW), and a battery percentage (pct) roughly estimated from voltage, not a precise fuel gauge. Works with a bare INA226 breakout wired directly to your sensor bus, or with a compatible add-on power monitor (e.g. M5Stack's Atomic Motion Base) on its own bus where supported — the firmware checks both automatically.",
  tcs34725: "AMS RGB colour sensor (I2C) — clear/red/green/blue raw counts, classified into a LEGO colour id.",
  as7341: "AMS 11-channel spectral sensor (I2C) — 8 visible spectral bands + clear + NIR, finer colour discrimination than tcs34725. The LED slider drives the breakout's illumination LED (0 = off).",
  vl53l1x: "ST time-of-flight distance sensor (I2C, bare chip) — configurable short (≤1.3m) or long (≤4m) ranging mode.",
  vl53l0x: "ST time-of-flight distance sensor, older generation (I2C, ≤2m) — same 0x29 address as vl53l1x/tcs34725 but a different protocol; the scan tells them apart by chip ID.",
  tof10120: "TOF10120 time-of-flight distance module (I2C, onboard MCU) — fixed-function, no ranging mode to configure.",
  tofi2c: "Generic TOF050/0200/0400-family distance module family (I2C, onboard MCU) — adjust register/scale in the recipe if your board differs.",
  gamepad: "Virtual sensor — a paired BLE gamepad's buttons/sticks, no physical wiring.",
  gpio: "Board DA-pin digital input — reads a pin's high/low state.",
  adc: "Board DA-pin analog input — reads a pin's raw ADC counts (or volts).",
  mcp3208: "MCP3208 8-channel 12-bit SPI ADC — pick the SPI CS for the chip and the channel (0-7) to read.",
  qre1113: "QRE1113 analogue reflectance sensor for line following, read via one or more MCP3208 channels (check several to read a sensor bar as one grouped sensor) — two-point white/black calibration shared across the group.",
  tssp_ir: "TSSP4038/TSOP34840 demodulating IR receiver for object tracking via 40kHz modulation, read via one or more MCP3208 channels (check several to read an IR ring as one grouped sensor) — reports object detected + signal strength per channel.",
  vk36n16: "Vinka VK36N16I 16-key capacitive touch keypad (I2C, default 0x65) — reports the touched key (0-15, -1 = none), the raw 16-bit multi-touch bitmap, and the touched-key count. Poll fast (20-50ms) or quick taps land between polls and are missed. If your board revision uses a different state register, set recipe reg (0 = default 0x00).",
  m5_8angle: "M5Stack 8Angle Unit (I2C, default 0x43) — 8 potentiometer knobs (12-bit raw counts each) plus the physical slide switch (0/1). The LED slider drives the per-knob RGB LEDs as a live value visualisation (blue = low, red = high; 0 = LEDs off) — only knobs that actually move rewrite their LED, so a steady panel adds no I2C traffic. This unit's I2C interface is handled by an onboard microcontroller rather than dedicated hardware and can become unresponsive under sustained rapid polling, so the poll rate floor is kept conservative — lower it further from the poll-rate dropdown if it stops responding to a scan after extended use.",
  m5_step16: "M5Stack Unit Step16 (I2C, default 0x48) — a 16-position detented rotary switch, reports the current step 0-15. The LED slider drives all the unit's lighting: the RGB ring as a live position visualisation (one colour for the whole ring: blue = 0 through red = 15) and the 7-segment digit (always shows the position 0-F — its content is fixed by the unit itself), both at that brightness; 0 = everything off. Writes only happen when the position or setting changes, so an untouched dial adds no I2C traffic. No pushbutton/press state is exposed by this unit's protocol. Shares its default address with ADS1115; the scan tells them apart by a heuristic, so double-check the guessed type. This unit's I2C interface is handled by an onboard microcontroller rather than dedicated hardware and can become unresponsive under sustained rapid polling, so the poll rate floor is kept conservative — lower it further from the poll-rate dropdown if it stops responding to a scan after extended use.",
};

export function emptyRecipe(): Recipe {
  return {
    reg: 0,
    length: 2,
    byte_order: "be",
    signed: false,
    scale: 1,
    offset: 0,
    value_names: ["value"],
  };
}

// Build a Sensor from a scanned device, with sensible defaults.
let nextId = 1;
// Does `s` already represent the physical device `d`? Identity is the wiring, not the type: the
// scan's `guess` is a hint the user can override when adding (a 0x76 reported as bmp280 added as
// a bme280 is still the same chip on the same pins), so matching on type would call that a
// different device and offer to add it a second time.
//
// Per bus, "the wiring" means:
//   i2c  — address plus which mux/channel it sits behind (0 / -1 when direct, matching how
//          sensorFromDiscovered defaults them, so a direct device compares equal either way)
//   spi  — the chip-select line; SPI has no addressing, so CS *is* the identity. A grouped
//          mcp3208-backed sensor (qre1113/tssp_ir reading several channels) shares one CS, which
//          is correct: the scan found one chip, and one sensor entry already covers it.
//   uart — the port.
// A builtin (the AtomS3R's onboard bmi270_bmm150) has no address or pins to compare at all, so
// its driver type is the only identity available.
export function sensorMatchesDiscovered(s: Sensor, d: Discovered): boolean {
  if (s.bus !== d.bus) return false;
  if (d.builtin) return s.type === d.guess;
  if (d.bus === "spi") return (s.cs_index ?? 0) === (d.cs_index ?? 0);
  if (d.bus === "uart") return (s.port ?? 0) === (d.port ?? 0);
  return (s.addr ?? 0) === (d.addr ?? 0) &&
         (s.mux_addr ?? 0) === (d.mux_addr ?? 0) &&
         (s.mux_channel ?? -1) === (d.channel ?? -1);
}

export function sensorFromDiscovered(d: Discovered, existing: Sensor[]): Sensor {
  const used = new Set(existing.map((s) => s.id));
  while (used.has(nextId)) nextId++;
  const id = nextId++;
  const type = d.guess && d.guess !== "unknown" ? d.guess : "generic";
  return {
    id,
    name: `${type}-${id}`,
    type,
    bus: d.bus,
    addr: d.addr ?? 0,
    mux_addr: d.mux_addr ?? 0,
    mux_channel: d.channel ?? -1,
    cs_index: d.cs_index ?? 0,
    port: d.port ?? 1,
    recipe: emptyRecipe(),
    transform: "raw",
    // tcs34725 starts pre-calibrated from a known-good real-unit capture instead of empty, so it
    // classifies reasonably before Calibrate/Teach — see defaultTcs34725Colours() above.
    calib: type === "tcs34725" ? [...TCS34725_PRESET_CALIB] : [],
    colours: type === "tcs34725" ? defaultTcs34725Colours() : undefined,
    poll_ms: 1000,
    enabled: true,
    simulate: false,
    show: false,
    page: 0,
    value_mask: 0xffff,
  };
}

// A `gamepad` sensor is virtual (no bus/pin) but still has to exist in the sensor list before
// its buttons/sticks can be picked as a LEGO emitter field source or shown on the dashboard —
// unlike scanned hardware, nothing on the bus prompts the user to add it. Used by the "+ Add
// gamepad sensor" shortcut in the Game controller card so pairing/enabling virtual mode doesn't
// leave the user hunting for a manual "+ Blank sensor" → change type to gamepad step.
export function newGamepadSensor(existing: Sensor[]): Sensor {
  const used = new Set(existing.map((s) => s.id));
  while (used.has(nextId)) nextId++;
  const id = nextId++;
  return {
    id,
    name: `gamepad-${id}`,
    type: "gamepad",
    bus: "i2c",
    recipe: emptyRecipe(),
    transform: "raw",
    calib: [],
    poll_ms: 50,
    enabled: true,
    simulate: false,
    show: false,
    page: 0,
    value_mask: 0xffff,
  };
}
