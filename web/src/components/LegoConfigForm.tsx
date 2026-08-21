import type { ReactNode } from "react";
import type { LegoBits, LegoConfig, LegoField, Reading, Sensor } from "../types";
import { HelpTip } from "./HelpTip";
import { Mascot } from "./Mascot";
import {
  GAMEPAD_BUTTONS,
  HUB_SUPPORTED_COLOUR_IDS,
  LEGO_COLOUR_NONE,
  LEGO_MAX_FIELDS,
  LEGO_PROFILE_COLOR,
  LEGO_PROFILE_MATRIX,
  LEGO_TARGET_COLOR,
  LEGO_TARGET_REFLT,
  LEGO_TARGET_RGBI,
  LEGO_TOTAL_BITS,
  isDistanceSensor,
  legoBitsUsed,
  packLego,
  rangeFromScale,
  scaleFromRange,
  sensorValueMeta,
  sensorValues,
  SPIKE_COLOURS,
} from "../types";
import { Palette, Lightbulb, Hash, Send, Gamepad2, ToyBrick, AlertTriangle, Grid3x3 } from "lucide-react";

interface Props {
  lego: LegoConfig;
  sensors: Sensor[];
  readings: Record<number, Reading>;
  streaming: boolean;
  matrix: string[] | null; // latest 3×3 Light Matrix pixels from the hub ("#rrggbb"×9)
  busy: string | null;
  // Kid/easy view (false) hides the UART wiring fields, the detailed per-field editor (target/
  // bits/signed/min-max/colour-map), and the binary-maths/hub-program exports — Quick assign +
  // the presets already cover what a kid needs to do here. Grown-up/advanced view shows all of it.
  advanced: boolean;
  board?: { name?: string; has_uart?: boolean } | null;
  onChange: (next: LegoConfig) => void;
  onSave: () => void;
}

const BITS_OPTIONS: LegoBits[] = [1, 2, 4, 8, 16];
const CHAN = ["R", "G", "B", "I"];

// A filler/padding field: no source sensor, always packs as 0. Its only job is reserving bits so
// a *following* field can be pushed to start at a channel boundary — no real sensor id is ever
// negative (config_store assigns/accepts non-negative ids), so this sentinel can never collide
// with a real sensor, and the firmware's cache lookup already returns 0 for an unknown id with
// no special-casing needed (see lego_emit.cpp's cache_lookup()).
const FILLER_SENSOR_ID = -1;
const isFiller = (f: LegoField) => f.sensor_id === FILLER_SENSOR_ID;
const fieldTarget = (f: LegoField) => f.target ?? LEGO_TARGET_RGBI;

// Value names for the sensor a field points at (empty if the sensor is gone).
function fieldValueNames(sensors: Sensor[], sensorId: number): string[] {
  const s = sensors.find((x) => x.id === sensorId);
  return s ? sensorValues(s) : [];
}

// Bitmask-shaped values (gamepad buttons/dpad) aren't a linearly-scalable quantity — "auto"'s
// proportional rescale (fit the catalogue's full range into however many bits the field has) is
// meant for continuous values like temperature or distance, where quantizing loses precision but
// stays meaningful. Applied to a bitmask it's actively wrong: e.g. `buttons` catalogued 0-65535
// squeezed into an 8-bit field auto-derives scale≈257, so `round(1/257) = 0` — pressing a single
// button (raw value 1) vanishes entirely. These need scale=1/offset=0 (copy the low N bits
// as-is) instead, matching how the bit-packer/word-block decode actually treat them.
function isBitmaskValue(sensors: Sensor[], sensorId: number, valueIndex: number): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  if (!s || s.type !== "gamepad") return false;
  const vname = fieldValueNames(sensors, sensorId)[valueIndex];
  // Only `buttons` is a true bitmask (one flag per bit — must keep all 16 bits or individual
  // buttons silently vanish). `dpad` is NOT: it's a small enumeration (0 = released, 1-8 =
  // hat direction), so it packs into 4 bits with identity scale — treating it as a bitmask
  // used to force a 16-bit field for a value that never exceeds 8.
  return vname === "buttons";
}

// Discrete-code values: small enumerations the hub-side code compares against exact numbers
// (a dpad direction, a boolean detected flag). Like bitmasks they must never be proportionally
// rescaled (dpad "up" = 1 must arrive as 1, not stretched across the field's range), but
// unlike bitmasks they can shrink to whatever width their value range fits in.
function isDiscreteCode(sensors: Sensor[], sensorId: number, valueIndex: number): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  if (!s) return false;
  const vname = fieldValueNames(sensors, sensorId)[valueIndex];
  if (s.type === "gamepad" && (vname === "dpad" || vname === "ldir" || vname === "rdir" ||
      /^(lx|ly|rx|ry)7$/.test(vname ?? "") || /^(lt|rt)15$/.test(vname ?? ""))) return true;
  return vname === "detected" || /^ch\d+_detected$/.test(vname ?? "");
}

// Gates the "map codes → values" offer on a REFLT/RGBI-target field (not COLOR — that one stays
// open to any value, mapping to a supported colour is its whole point) to just the gamepad's
// digital dpad/stick codes: dpad, its compass-encoded ldir/rdir, and the quantized ±7 stick axes
// (pad_digital transform). These are a small, genuinely fixed set of states a code→value table
// suits. A line sensor's detected flag, a distance sensor's near/far, or a plain counter are
// continuous/derived readings, not a fixed enumeration — offering a 16-entry map there invites
// building a table for states that don't really exist.
function isGamepadDigitalCode(sensors: Sensor[], sensorId: number, valueIndex: number): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  if (!s || s.type !== "gamepad") return false;
  const vname = fieldValueNames(sensors, sensorId)[valueIndex];
  return vname === "dpad" || vname === "ldir" || vname === "rdir" || /^(lx|ly|rx|ry)7$/.test(vname ?? "");
}

// colour/reflect/r/g/b from a "colour + reflect + RGB" sensor (col_full/as_full) are already the
// exact values a real LEGO colour sensor's native COLOR/REFLT/RGB-I slots carry (0-10, 0-100,
// 0-1024) — real passthrough sends them completely unscaled (see combo_value()/passthrough_get()
// in lego_emit.cpp, no proportional stretch anywhere). Like a bitmask, these shouldn't be
// proportionally rescaled to fill a wider field — that would send a number the hub interprets
// completely differently from the real thing (e.g. colour id 6 becoming byte value 162).
function isNativeColourValue(sensors: Sensor[], sensorId: number, valueIndex: number): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  if (!s || (s.transform !== "col_full" && s.transform !== "as_full")) return false;
  // colour, reflect, r, g, b — always this order. Deliberately excludes the 6th value (raw
  // clear, index 5): unlike the other 5, it's not a value the hub interprets in one fixed native
  // format — it's just a wide raw count (0-65535) passthrough never sends, so a proportional
  // auto-fit into whatever field width you give it is the right default, same as any other
  // continuous sensor value.
  return valueIndex >= 0 && valueIndex <= 4;
}

// Only the literal classified colour id (index 0 of col_full/as_full) is already a native LEGO
// colour — reflect/r/g/b (indices 1-4) are continuous readings that just happen to share
// isNativeColourValue()'s "send unscaled" treatment above. Conflating the two used to also hide
// "map codes → colours" for reflect/r/g/b whenever they were routed through a COLOR-target
// field (it reused isNativeColourValue, so a colour sensor's reflect%/r/g/b never got the
// option) even though only the actual colour id needs no remapping. Used for that gating only —
// defaultScaleOffset above deliberately keeps using the broader isNativeColourValue.
// Whether `valueIndex` is already the exact value real LEGO hardware puts in `target`'s slot —
// the classified colour id in COLOR (index 0), reflect% in REFLT (index 1), or r/g/b in RGBI
// (indices 2-4) — for a col_full/as_full sensor. Only these specific (target, index) pairs are
// already correct as-is; mapping is just as useful for every other value/target combination,
// including these same 5 values routed to a *different* slot than their natural one.
function isNativeSlotValue(sensors: Sensor[], sensorId: number, valueIndex: number, target: number): boolean {
  if (!isNativeColourValue(sensors, sensorId, valueIndex)) return false;
  if (target === LEGO_TARGET_COLOR) return valueIndex === 0;
  if (target === LEGO_TARGET_REFLT) return valueIndex === 1;
  if (target === LEGO_TARGET_RGBI) return valueIndex >= 2 && valueIndex <= 4;
  return false;
}
// Small integer codes where -1 is a "nothing" sentinel rather than a real value: the classified
// LEGO colour id (col_lego/as_lego/col_full/as_full index 0) and the vk36n16's key code. Like
// bitmasks and discrete codes these must never be proportionally rescaled — a colour id IS the
// number the hub looks up, so stretching it changes which colour is reported (id 6 "green" fitted
// across a 4-bit field became raw 10, which the hub reads as white). col_full/as_full were
// already covered by isNativeColourValue, but the dedicated col_lego/as_lego "LEGO colour id"
// modes — the obvious thing to pick for this — were not, and their -1..11 range fails
// fitsIdentity on the negative minimum, so they fell through to exactly that proportional fit.
//
// Deliberately NOT folded into isDiscreteCode(): changeFieldSource() turns a discrete code with a
// negative minimum into a *signed* field, which for these would be wrong — a signed 4-bit field
// tops out at 7 and would clip colour ids 8-11. The -1 is a sentinel the COLOR byte carries as
// 255 (see the firmware's current_color_reflt), not a value needing a sign bit.
function isCodeIdValue(sensors: Sensor[], sensorId: number, valueIndex: number): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  if (!s) return false;
  if (s.type === "vk36n16") return fieldValueNames(sensors, sensorId)[valueIndex] === "key";
  const tf = s.transform;
  if (tf === "col_lego" || tf === "as_lego") return valueIndex === 0;
  if (tf === "col_full" || tf === "as_full") return valueIndex === 0;
  return false;
}

// Identity scaling is only lossless for integer-valued readings. A continuous value (ModeValue
// .cont — reflectance, IR strength, volts) must always be fitted proportionally, however
// comfortably its range fits the field's raw capacity: 0-1 reflectance "fitting" a 1-bit field
// means every reading collapses to 0 or 1, and 0-3.3V into 4 bits gives four steps.
function identityAllowed(sensors: Sensor[], sensorId: number, valueIndex: number,
                         min: number, max: number, bits: number, signed: boolean): boolean {
  const s = sensors.find((x) => x.id === sensorId);
  const meta = s ? sensorValueMeta(s, valueIndex) : undefined;
  if (meta?.cont) return false;
  return fitsIdentity(min, max, bits, signed);
}

// Whether [min, max] already fits the field's raw integer capacity as-is (identity: raw =
// round(value), no scale/offset needed) — unsigned raw ranges 0..2^bits-1, signed
// -2^(bits-1)..2^(bits-1)-1. If it fits, sending it unscaled is both simpler (no float multiply
// in the exported Word block/Python — see WordBlocksView) and loses nothing versus stretching:
// every value the sensor can actually produce still lands on its own distinct raw code. Only
// once the range would overflow the field does a proportional fit (scaleFromRange) become
// necessary to make it fit at all.
function fitsIdentity(min: number, max: number, bits: number, signed: boolean): boolean {
  if (signed) {
    const half = 1 << (bits - 1);
    return min >= -half && max <= half - 1;
  }
  return min >= 0 && max <= (1 << bits) - 1;
}

// Best-effort recovery of the value range a field is currently configured to send. Usually just
// the exact inverse of scale/offset (rangeFromScale) — but defaultScaleOffset leaves scale=1/
// offset=0 ("send the raw value as-is") whenever the value's own catalogue range already fits
// inside the field's raw capacity (see defaultScaleOffset), and that identity mapping carries no
// memory of the *value's* real max: inverting it naively reports the field's whole raw capacity
// (e.g. 65535 for a 16-bit field) instead of the value's actual range (e.g. 0-4000). That
// inflated number then looks frozen across every subsequent bits change, since each change
// re-derives from it instead of from the value's real range.
//
// Only take the catalogue-range shortcut when the field is *still at its auto-derived default*
// for this bit width (scale/offset exactly matching what defaultScaleOffset would produce) —
// not just whenever scale/offset happen to equal 1/0. A field the user hand-typed a custom
// min/max into can *also* land on scale=1/offset=0 (setRange calls fitsIdentity with the field's
// own current `signed` flag, which a hand-typed signed range hits more easily than the
// unsigned-only checks defaultScaleOffset itself performs) — in that case scale=1/offset=0 is
// the user's own deliberate choice, and silently snapping back to the sensor's unrelated
// catalogue default would clobber it instead of fixing anything.
function currentRange(sensors: Sensor[], f: LegoField, bits: number, signed: boolean): { min: number; max: number } {
  const d = defaultScaleOffset(sensors, f.sensor_id, f.value_index, bits, signed);
  if (f.scale === d.scale && f.offset === d.offset) {
    const s = sensors.find((x) => x.id === f.sensor_id);
    const meta = s ? sensorValueMeta(s, f.value_index) : undefined;
    if (meta) return { min: meta.min, max: meta.max };
  }
  return rangeFromScale(f.scale, f.offset, bits, signed);
}

// Default scale/offset for a field: 1/0 (send the value as-is) for bitmasks and native colour
// values (these must never be proportionally rescaled, regardless of fit — see isBitmaskValue/
// isNativeColourValue), also 1/0 whenever the value's own range already fits the field's raw
// capacity (fitsIdentity), and otherwise a proportional fit of the value's catalogue range into
// the field's width (the only case where scaling is actually needed to avoid clipping).
// `signed` (default false, matching every call site that doesn't yet track a field's own signed
// flag) must reflect the field's *actual* signed-ness whenever one is known — fitsIdentity's and
// scaleFromRange's signed raw-capacity/offset math genuinely differ from the unsigned case, so
// hard-coding false here would derive a mapping for the wrong wire layout the moment a field is
// (or becomes) signed.
function defaultScaleOffset(sensors: Sensor[], sensorId: number, valueIndex: number, bits: number, signed = false): { scale: number; offset: number } {
  if (isBitmaskValue(sensors, sensorId, valueIndex) || isDiscreteCode(sensors, sensorId, valueIndex) ||
      isNativeColourValue(sensors, sensorId, valueIndex) || isCodeIdValue(sensors, sensorId, valueIndex)) {
    return { scale: 1, offset: 0 };
  }
  const s = sensors.find((x) => x.id === sensorId);
  const meta = s ? sensorValueMeta(s, valueIndex) : undefined;
  if (!meta) return { scale: 1, offset: 0 };
  if (identityAllowed(sensors, sensorId, valueIndex, meta.min, meta.max, bits, signed)) return { scale: 1, offset: 0 };
  return scaleFromRange(meta.min, meta.max, bits, signed);
}

// The smallest of the available field widths (1/2/4/8/16 bits) that keeps a reasonable number of
// distinguishable steps across the value's whole catalogue range — e.g. a colour id (-1..10, 12
// states) fits comfortably in 4 bits, a percentage (0..100) needs 8, but a wide raw count or a
// value already packed at full native width doesn't gain anything from shrinking. This only sets
// the *default* when you pick a sensor/value — the bits dropdown is still yours to override, e.g.
// a distance sensor's full mm range technically wants 16 bits, but many uses don't need mm
// precision, so trading it for 8 bits frees real budget for other fields (auto-derives the
// scale/offset to fit whichever width you land on either way). A bitmask (buttons/dpad) always
// keeps 16 — shrinking it would silently drop individual button bits, not just precision.
// `hasColourMap`: once a code→value map is active, the field no longer carries the value's own
// small range — it carries whatever's in the map, including MC_LEGO_COLOUR_NONE (255) for an
// unmapped code. A field sized to the *code's* range (e.g. dpad's 4 bits, 0-15) would silently
// clamp that 255 down to 15 — indistinguishable from a code deliberately mapped to 15. Needs a
// full 8 bits regardless of how few bits the raw code itself would fit in.
function suggestedBits(sensors: Sensor[], sensorId: number, valueIndex: number, hasColourMap?: boolean): LegoBits {
  if (hasColourMap) return 8;
  if (isBitmaskValue(sensors, sensorId, valueIndex)) return 16;
  const s = sensors.find((x) => x.id === sensorId);
  // Distance sensors default to 8 bits regardless of their span: their native range (often
  // 1000s of mm) would otherwise always fall through to 16 below, but few uses actually need
  // mm-level precision — an 8-bit field across a 4000mm range is still ~16mm resolution, and
  // freeing those 8 bits is usually worth more than the precision (e.g. lets 2 more sensors fit
  // in the 64-bit budget). Still just a default — pick 16 back if you do need finer steps.
  if (s && isDistanceSensor(s)) return 8;
  const meta = s ? sensorValueMeta(s, valueIndex) : undefined;
  if (!meta) return 16;
  // Continuous values must be sized by the resolution they deserve, not by their span: the rules
  // below read a span of 1 as "boolean" and a span of 3.3 as "4 bits", which is right for a
  // detected flag or a dpad but gives a 0-1 reflectance a single bit and 0-3.3V four steps. 8
  // bits is 256 steps across whatever the range is — plenty for these, and still cheap enough in
  // the 64-bit budget to be a sane default the dropdown can widen to 16.
  if (meta.cont) return 8;
  const span = meta.max - meta.min;
  if (span <= 1) return 1;     // boolean flag (detected, gpio state)
  if (span <= 3) return 2;
  if (span <= 15) return 4;    // dpad (0-8), colour id (-1..10), key codes
  if (span <= 255) return 8;
  return 16;
}

// Bit-width choices offered for a specific value. Continuous values (distance, temperature)
// keep the full list — a wider field genuinely buys resolution, since the auto-scale spreads
// the value's range across however many steps the width provides. Identity-scaled values
// (bitmasks, discrete codes, native colour slots) gain nothing from extra width — the number
// is sent as-is, so any bit beyond the value's own range is pure wasted budget — and lose
// meaning when truncated below it; they get exactly the widths that fit:
//   buttons          → 16 only (a true bitmask; fewer drops individual buttons)
//   dpad (0-8)       → 4 only          detected flag (0/1) → 1 only
//   colour id (0-10) → 4 only          reflect (0-100)     → 8 only, etc.
// A stale config width outside the list is still shown so the dropdown never lies about the
// saved state.
function bitsOptionsFor(sensors: Sensor[], sensorId: number, valueIndex: number, current: LegoBits, hasColourMap?: boolean): LegoBits[] {
  let opts: LegoBits[];
  if (hasColourMap) {
    // See suggestedBits() — a mapped value can be MC_LEGO_COLOUR_NONE (255), so nothing
    // narrower than 8 bits is offered regardless of the underlying code's own range.
    opts = [8, 16];
  } else if (isBitmaskValue(sensors, sensorId, valueIndex)) {
    opts = [16];
  } else if (isDiscreteCode(sensors, sensorId, valueIndex) || isNativeColourValue(sensors, sensorId, valueIndex) ||
             isCodeIdValue(sensors, sensorId, valueIndex)) {
    const s = sensors.find((x) => x.id === sensorId);
    const meta = s ? sensorValueMeta(s, valueIndex) : undefined;
    // A code-id value's -1 is a sentinel, not a magnitude to reserve a bit for — sizing from
    // |min| would push a 0..11 colour id to a wider field for a value it never actually sends.
    const floorMag = meta ? (isCodeIdValue(sensors, sensorId, valueIndex) ? 0 : Math.abs(meta.min)) : 0;
    const needed = meta
      ? (BITS_OPTIONS.find((b) => (1 << b) - 1 >= Math.max(meta.max, floorMag)) ?? 16)
      : 16;
    opts = [needed];
  } else {
    opts = BITS_OPTIONS;
  }
  return opts.includes(current) ? opts : [...opts, current].sort((a, b) => a - b) as LegoBits[];
}

// How many code slots a COLOR-target code→colour map needs for this value (its range, capped
// at the firmware's 16-entry table).
function colourMapCodes(sensors: Sensor[], f: LegoField): number {
  const s = sensors.find((x) => x.id === f.sensor_id);
  const meta = s ? sensorValueMeta(s, f.value_index) : undefined;
  const maxCode = meta ? Math.min(15, Math.max(1, Math.round(meta.max))) : 15;
  return maxCode + 1;
}

// Friendly per-code labels for the map editor — direction names for dpad/stick-compass codes,
// on/off for boolean-shaped values (any *_detected flag, line_reflect's plain "detected", a
// gpio/generic "state"), plain numbers otherwise. Not just for colour sensors — "map codes →
// colours" works on any value a LEGO field can read (reflect/detected/dpad/buttons/...), so
// these non-colour-sensor code sets deserve the same readable labelling the colour sensor's own
// classified colour ids already got.
const DIR_NAMES = ["·", "N", "NE", "E", "SE", "S", "SW", "W", "NW"];
function codeLabel(valueName: string | undefined, code: number): string {
  if ((valueName === "dpad" || valueName === "ldir" || valueName === "rdir") && code < DIR_NAMES.length)
    return code === 0 ? (valueName === "dpad" ? "off" : "centre") : DIR_NAMES[code];
  if (valueName && (valueName === "state" || /detected$/.test(valueName)) && code <= 1)
    return code === 0 ? "off" : "on";
  return String(code);
}

// Which RGBI channel(s) a field lands in, from its cumulative bit offset among the *other*
// RGBI-target fields before it (packing is positional: 16 bits per channel, R then G then B
// then I) — a COLOR/REFLT-target field doesn't occupy any RGBI channel at all (it's a separate
// byte the hub can request independently, see LEGO_TARGET_* in types.ts), and doesn't consume
// space that would shift where later RGBI fields land either.
function channelLabel(fields: LegoField[], idx: number): string {
  if (fieldTarget(fields[idx]) === LEGO_TARGET_COLOR) return "COLOR";
  if (fieldTarget(fields[idx]) === LEGO_TARGET_REFLT) return "REFLT";
  let off = 0;
  for (let i = 0; i < idx; i++) if (fieldTarget(fields[i]) === LEGO_TARGET_RGBI) off += fields[i].bits;
  const start = Math.floor(off / 16);
  const end = Math.floor((off + fields[idx].bits - 1) / 16);
  if (start > 3) return "—";
  return start === end ? CHAN[start] : `${CHAN[start]}–${CHAN[Math.min(end, 3)]}`;
}

// Round for display without trailing float noise.
const tidy = (n: number): number => Math.round(n * 1e4) / 1e4;

// Sanitise a label into a Python identifier for the generated decoder.
function pyIdent(s: string): string {
  const id = s.replace(/[^a-zA-Z0-9_]/g, "_").replace(/^(\d)/, "_$1");
  return id || "field";
}

// Python-safe identifier for a GAMEPAD_BUTTONS entry — most names (A, B, LB, View, ...) are
// already valid via pyIdent, but the 4 dpad-direction entries are arrow glyphs (↑↓←→) that all
// collapse to the same "_" under pyIdent's non-alnum stripping, colliding with each other.
function gamepadBitName(bit: number, label: string): string {
  const dpad: Record<number, string> = { 12: "dup", 13: "ddown", 14: "dleft", 15: "dright" };
  return dpad[bit] ?? pyIdent(label);
}

// The decode() function for the current field layout. The firmware packs fields LSB-first
// into a 64-bit word split across the 4 RGBI channels; this reverses it. Shared by both the
// SPIKE and Pybricks programs below. A field sourced from a `gamepad` sensor's `buttons` or
// `dpad` value additionally expands into named booleans / a direction string — otherwise the
// hub program only gets an opaque bitmask/hat number with no indication what each bit means.
function decodeLines(fields: LegoField[], sensors: Sensor[]): string[] {
  const lines: string[] = [];
  lines.push("def decode(r, g, b, i):");
  lines.push("    # rebuild the 64-bit word (mask each channel — DATA16 can come back signed)");
  lines.push("    w = (r & 0xFFFF) | ((g & 0xFFFF) << 16) | ((b & 0xFFFF) << 32) | ((i & 0xFFFF) << 48)");
  lines.push("    out = {}");
  if (fields.length === 0) lines.push("    # (no fields configured yet)");
  let off = 0;
  const used = new Set<string>();
  fields.forEach((f, idx) => {
    if (fieldTarget(f) !== LEGO_TARGET_RGBI) return;   // COLOR/REFLT — read separately, see colorReflectFields()
    if (isFiller(f)) { off += f.bits; return; }   // padding — no value to decode, just skip its bits
    const s = sensors.find((x) => x.id === f.sensor_id);
    const vname = fieldValueNames(sensors, f.sensor_id)[f.value_index] ?? `v${f.value_index}`;
    let name = pyIdent(`${s ? s.name : "s" + f.sensor_id}_${vname}`);
    while (used.has(name)) name = `${name}_${idx}`;
    used.add(name);
    const mask = "0x" + (2 ** f.bits - 1).toString(16);
    lines.push(`    raw = (w >> ${off}) & ${mask}   # ${f.bits}-bit`);
    if (f.signed) lines.push(`    if raw >= ${2 ** (f.bits - 1)}: raw -= ${2 ** f.bits}`);
    const scale = f.scale === 0 ? 1 : f.scale;
    if (scale === 1 && f.offset === 0) lines.push(`    out["${name}"] = raw`);
    else lines.push(`    out["${name}"] = raw * ${scale} + ${f.offset}`);

    if (s?.type === "gamepad" && vname === "buttons") {
      lines.push(`    # ${name}: individual button booleans (bit → name, see docs/hid-gamepad.md)`);
      for (const btn of GAMEPAD_BUTTONS) {
        const bitName = pyIdent(`${name}_${gamepadBitName(btn.bit, btn.name)}`);
        lines.push(`    out["${bitName}"] = bool(raw & 0x${(1 << btn.bit).toString(16)})`);
      }
    }
    if (s?.type === "gamepad" && vname === "dpad") {
      lines.push(`    _dpad_dir = ["center", "up", "up_right", "right", "down_right", "down", "down_left", "left", "up_left"]`);
      lines.push(`    out["${name}_dir"] = _dpad_dir[raw] if 0 <= raw <= 8 else "center"`);
    }

    off += f.bits;
  });
  lines.push("    return out");
  return lines;
}

// Fields targeting COLOR/REFLT (see LEGO_TARGET_* in types.ts) live outside the RGBI word
// entirely — the hub reads them via separate calls (color_sensor.color()/.reflection(), or
// device.read(0)/(1) in Pybricks), not by unpacking r/g/b/i. Only the *last* field targeting
// each one is what the firmware actually sends (config_store keeps just one), matching here.
interface ColorReflectField { name: string; target: number; scale: number; offset: number }
function colorReflectFields(fields: LegoField[], sensors: Sensor[]): ColorReflectField[] {
  const out: ColorReflectField[] = [];
  fields.forEach((f) => {
    const target = fieldTarget(f);
    if (target === LEGO_TARGET_RGBI || isFiller(f)) return;
    const s = sensors.find((x) => x.id === f.sensor_id);
    const vname = fieldValueNames(sensors, f.sensor_id)[f.value_index] ?? `v${f.value_index}`;
    const name = pyIdent(`${s ? s.name : "s" + f.sensor_id}_${vname}`);
    const existing = out.findIndex((e) => e.target === target);
    const entry = { name, target, scale: f.scale === 0 ? 1 : f.scale, offset: f.offset };
    if (existing >= 0) out[existing] = entry; else out.push(entry);   // last one targeting a slot wins
  });
  return out;
}

// ── Quick assign: the 6 destinations the hub can actually see (COLOR, REFLT, R, G, B, I),
// always shown together so "what's mapped where" doesn't depend on reading field order/channel
// badges. Only recognises the *simple* shape — one field cleanly filling a whole slot (a whole
// 16-bit RGBI channel, or the one COLOR/REFLT byte) — sub-channel/multi-field-per-channel setups
// built by hand in the detailed editor below show as unrecognised here (edit them down there
// instead; picking a value here for that slot replaces them with the simple shape). ──────────
type QuickSlot = "color" | "reflect" | 0 | 1 | 2 | 3;
const QUICK_SLOTS: { slot: QuickSlot; label: string }[] = [
  { slot: "color", label: "COLOR" },
  { slot: "reflect", label: "REFLT" },
  { slot: 0, label: "R" },
  { slot: 1, label: "G" },
  { slot: 2, label: "B" },
  { slot: 3, label: "I" },
];
const QUICK_UNUSED = -2;   // distinct from FILLER_SENSOR_ID (-1) — "nothing assigned to this slot"

function findQuickField(fields: LegoField[], slot: QuickSlot): LegoField | undefined {
  if (slot === "color") { const m = fields.filter((f) => fieldTarget(f) === LEGO_TARGET_COLOR); return m[m.length - 1]; }
  if (slot === "reflect") { const m = fields.filter((f) => fieldTarget(f) === LEGO_TARGET_REFLT); return m[m.length - 1]; }
  // The value field itself no longer has to be a full 16 bits (setQuickSlot pads the rest of the
  // channel with filler at its suggested width) — recognise any non-filler RGBI field that starts
  // clean at the top of the channel, whatever its width.
  return fields.find((f, i) => {
    if (isFiller(f) || fieldTarget(f) !== LEGO_TARGET_RGBI) return false;
    const { channelStart, channelEnd, offsetInStart } = fieldChannelInfo(fields, i);
    return channelStart === slot && channelEnd === slot && offsetInStart === 0;
  });
}

// ── Word-block diagram pieces (styled to match the Pybricks/SPIKE Blockly palette: rounded pill
// blocks, white oval "sockets" for whatever plugs into them) ────────────────────────────────────
function Socket({ children }: { children: ReactNode }) {
  return <span className="op-socket">{children}</span>;
}
// A two-socket Operators block, e.g. "( ) / ( )" or "( ) mod ( )".
function OpBlock({ a, op, b }: { a: ReactNode; op: string; b: ReactNode }) {
  return (
    <span className="op-block">
      <Socket>{a}</Socket>
      <span className="op-word">{op}</span>
      <Socket>{b}</Socket>
    </span>
  );
}
// The "(dropdown ▾) of ( )" Operators block (abs/floor/ceiling/sqrt/...).
function FuncBlock({ fn, arg }: { fn: string; arg: ReactNode }) {
  return (
    <span className="op-block">
      <span className="op-dropdown">{fn} ▾</span> of <Socket>{arg}</Socket>
    </span>
  );
}
function ReporterBlock({ label }: { label: string }) {
  return <span className="reporter-block">{label}</span>;
}
// A Variables-category "set [name] to ( )" block.
function SetVarBlock({ name, value }: { name: string; value: ReactNode }) {
  return (
    <div className="var-block">
      set <span className="var-name">{name}</span> to <Socket>{value}</Socket>
    </div>
  );
}

// Which RGBI channel a field lands in and its bit offset *within* that channel — needed to know
// whether it can be read straight off a reporter block (a whole 16-bit field, channel-aligned)
// or needs unpacking with math (a smaller field sharing a channel with others), and whether it
// spans a channel boundary (packing is positional, so a field doesn't have to align — rare, but
// there's no clean word-block way to combine two reporter blocks bit-for-bit).
function fieldChannelInfo(fields: LegoField[], idx: number): { channelStart: number; channelEnd: number; offsetInStart: number } {
  let off = 0;
  for (let i = 0; i < idx; i++) if (fieldTarget(fields[i]) === LEGO_TARGET_RGBI) off += fields[i].bits;
  const channelStart = Math.floor(off / 16);
  const channelEnd = Math.floor((off + fields[idx].bits - 1) / 16);
  return { channelStart, channelEnd, offsetInStart: off % 16 };
}

// Zero-padded base-2 string of `raw` within its field's exact bit width — masked first so a
// signed field's two's-complement bit pattern (what's actually on the wire) shows correctly
// rather than a huge negative-number string.
function toBinary(raw: number, bits: number): string {
  const mask = (2 ** bits) - 1;
  return ((raw & mask) >>> 0).toString(2).padStart(bits, "0");
}

// Which RGBI channel (R/G/B/I) each bit of a field belongs to, MSB-first — matching toBinary()'s
// string order — so a field that happens to straddle a channel boundary still gets each half
// coloured correctly instead of one single colour for the whole field.
function bitChannels(fieldBits: number, absStart: number): (typeof CHAN)[number][] {
  const out: (typeof CHAN)[number][] = [];
  for (let bitIdx = fieldBits - 1; bitIdx >= 0; bitIdx--) {
    out.push(CHAN[Math.min(Math.floor((absStart + bitIdx) / 16), 3)]);
  }
  return out;
}

function BitStrip({ bits, channels, extra }: { bits: string; channels: string[]; extra?: ReactNode }) {
  const groups = bits.match(/.{1,4}/g) ?? [bits];   // 4 bits per group (a nibble) — easier to count
  let pos = 0;
  return (
    <div className="bit-strip">
      {groups.map((g, gi) => {
        const start = pos;
        pos += g.length;
        return (
          <span key={gi} className="bit-group">
            {g.split("").map((bit, bi) => (
              <span key={bi} className={`bit ${bit === "1" ? "bit-on" : "bit-off"} chan-${channels[start + bi]}`}>{bit}</span>
            ))}
          </span>
        );
      })}
      {extra}
    </div>
  );
}

// Binary-maths explainer, written for a beginner (the kind of person building the Word-blocks
// program below, not necessarily anyone who's seen binary before): for each field, shows the sum
// that turns its live reading into the whole number ("raw") actually sent, that number spelled
// out one bit at a time, and where those bits land inside the 64-bit word the hub receives as
// four 16-bit channels (R, G, B, I) — the same split the BUDGET bar at the top counts against.
// Deliberately separate from WordBlocksView (which is about *using* the number in a program):
// this is about *why* the number on the wire looks the way it does.
function BinaryMathsView({ fields, sensors, packed }: { fields: LegoField[]; sensors: Sensor[]; packed: ReturnType<typeof packLego> }) {
  if (fields.length === 0) return <p className="muted sm">No fields configured yet.</p>;
  const rgbiFields = fields.map((f, i) => ({ f, i })).filter(({ f }) => fieldTarget(f) === LEGO_TARGET_RGBI && !isFiller(f));

  return (
    <div className="bit-maths">
      <p className="muted sm">
        A computer only ever stores <b>0</b>s and <b>1</b>s — <i>bits</i>. The hub receives four
        16-bit numbers (R, G, B, I) from the sensor, so there are <b>4 × 16 = 64 bits</b> total to
        share out — the same 64 the <b>BUDGET</b> bar above counts down from. Every field claims
        some of those bits for one value; here's the sum and the actual 1s and 0s for each one.
      </p>
      {rgbiFields.map(({ f, i }) => {
        const row = packed.rows[i];
        if (!row) return null;
        const { channelStart, channelEnd, offsetInStart } = fieldChannelInfo(fields, i);
        const s = sensors.find((x) => x.id === f.sensor_id);
        const vname = (s ? sensorValues(s) : [])[f.value_index] ?? `v${f.value_index}`;
        const label = s ? `${s.name}.${vname}` : `#${f.sensor_id}.${vname}`;
        const scale = f.scale === 0 ? 1 : f.scale;
        const identity = scale === 1 && f.offset === 0;
        const chanText = channelStart === channelEnd
          ? `channel ${CHAN[Math.min(channelStart, 3)]}`
          : `channels ${CHAN[channelStart]}–${CHAN[Math.min(channelEnd, 3)]}`;
        return (
          <div key={i} className="bit-row">
            <div className="bit-row-head">
              <b>{label}</b>
              <span className="muted sm">
                {f.bits}-bit field{f.signed ? " (signed)" : ""} → {chanText}, starting at bit {offsetInStart} of that channel
              </span>
            </div>
            <div className="muted sm">
              {identity
                ? <>raw = value = <b>{tidy(row.value)}</b> — no scaling needed, it already fits in {f.bits} bits as-is</>
                : <>raw = round((value − offset) ÷ scale) = round(({tidy(row.value)} − {tidy(f.offset)}) ÷ {tidy(scale)}) = <b>{row.raw}</b></>}
            </div>
            <BitStrip
              bits={toBinary(row.raw, f.bits)}
              channels={bitChannels(f.bits, channelStart * 16 + offsetInStart)}
              extra={<span className="muted sm">= {row.raw} in decimal</span>}
            />
          </div>
        );
      })}
      <div className="bit-row">
        <div className="bit-row-head"><b>The whole word, channel by channel</b></div>
        <p className="muted sm" style={{ margin: 0 }}>
          Every field's bits above get lined up end-to-end (the first field starts at bit 0), then
          split into four 16-bit chunks — that's the R, G, B, I the hub actually receives. Each
          row below is coloured to match that channel, same as the field strips above.
        </p>
        {(["R", "G", "B", "I"] as const).map((c, ci) => (
          <div key={c} style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <span className="muted sm" style={{ width: 14 }}>{c}</span>
            <BitStrip
              bits={toBinary(packed.channels[ci], 16)}
              channels={Array(16).fill(c)}
              extra={<span className="muted sm">= {packed.channels[ci]}</span>}
            />
          </div>
        ))}
      </div>
    </div>
  );
}

const RGBI_REPORTER = ["🟥 red light (advanced blocks)", "🟩 green light (advanced blocks)", "🟦 blue light (advanced blocks)", null] as const;

// Word-block (Pybricks/SPIKE Blockly) explainer. A field CAN be decoded with word blocks — it
// just takes a few Operators blocks (÷, mod, floor) instead of Python's `>>`/`&`, the same way
// the generated Python decode() does it: raw = floor(channelValue / 2^offset) mod 2^bits. The
// real limitation (per LEGO's advanced Color Sensor blocks) is that only the red/green/blue
// channels have a reporter block at all — there's no block for the 4th "clear"/intensity
// channel, so a field packed there is Python-only regardless of the maths. Channel-aligned
// 16-bit fields (one whole field per RGB channel, no sharing) don't need the maths at all — just
// read the reporter directly, which is the simplest layout for word-block use if you're setting
// fields up from scratch.
function WordBlocksView({ lego, fields, sensors, showRealValue }: { lego: LegoConfig; fields: LegoField[]; sensors: Sensor[]; showRealValue: boolean }) {
  const source = sensors.find((s) => s.id === lego.colour_source);

  if (source) {
    return (
      <div className="blocks-stack">
        <div className="hat-block">▶ when program starts</div>
        <div className="loop-block">
          {/* Only the id/reflect blocks carry a leading icon — Palette/Lightbulb distinguish two
              *concepts* (colour-id vs. reflectance), not colours. The red/green/blue/clear blocks
              below already say their channel via the block's own data-colour background (see
              .snap-block[data-colour] in styles.css) — a 🟥/🟩/🟦/⬜ prefix there was just
              repeating the same signal the block colour already gives, and ⬜'s "white" didn't
              even match the clear channel's actual grey background. */}
          <div className="snap-block" data-colour="id">
            <Palette size={14} strokeWidth={2.25} className="inline-icon" /> <span className="reporter">{source.name}: color</span> → LEGO colour id (0–10, −1 = none)
          </div>
          <div className="snap-block" data-colour="reflect">
            <Lightbulb size={14} strokeWidth={2.25} className="inline-icon" /> <span className="reporter">{source.name}: reflected light %</span> → 0–100
          </div>
          <div className="snap-block" data-colour="red">
            <span className="reporter">{source.name}: red light</span> → 0–1024 (advanced blocks)
          </div>
          <div className="snap-block" data-colour="green">
            <span className="reporter">{source.name}: green light</span> → 0–1024 (advanced blocks)
          </div>
          <div className="snap-block" data-colour="blue">
            <span className="reporter">{source.name}: blue light</span> → 0–1024 (advanced blocks)
          </div>
          <div className="snap-block disabled" data-colour="clear">
            clear / intensity — no reporter block for the raw count, but see below
          </div>
        </div>
        <span className="muted sm">
          Repeat inside a <b>forever</b> loop and compare/store each reporter like any other sensor block.
          There's no reporter for the raw clear/intensity count specifically — but{" "}
          <b>reflected light %</b> above <i>is</i> that same signal, just white-balanced and
          scaled 0–100 instead of a raw count, so it already covers "how much light is coming
          back" for word blocks. The raw count is only reachable from Python's{" "}
          <code>color_sensor.rgbi()</code> (its 4th value) if you specifically need it
          uncalibrated.
        </span>
      </div>
    );
  }

  if (fields.length === 0) {
    return <p className="muted sm" style={{ margin: 0 }}>No data selected yet — add a field above, or turn on colour passthrough for the simplest word-block reporters.</p>;
  }

  return (
    <div className="blocks-stack">
      {fields.map((f, i) => {
        if (isFiller(f)) return null;   // padding — nothing to read, no block for it
        const vname = fieldValueNames(sensors, f.sensor_id)[f.value_index] ?? `v${f.value_index}`;
        const s = sensors.find((x) => x.id === f.sensor_id);
        const varName = `${s ? s.name : "s" + f.sensor_id}_${vname}`;

        // COLOR/REFLT-target fields bypass the RGBI word entirely (see LEGO_TARGET_* in
        // types.ts) — read straight off the matching native reporter block, no unpacking maths
        // needed at all (that's the whole point of these two targets).
        const target = fieldTarget(f);
        if (target === LEGO_TARGET_COLOR || target === LEGO_TARGET_REFLT) {
          const scale = f.scale === 0 ? 1 : f.scale;
          let value: ReactNode = <ReporterBlock label={target === LEGO_TARGET_COLOR ? "color" : "reflected light %"} />;
          if (scale !== 1) value = <OpBlock a={value} op="×" b={scale} />;
          if (f.offset !== 0) value = <OpBlock a={value} op="+" b={f.offset} />;
          return <SetVarBlock key={i} name={varName} value={value} />;
        }

        const { channelStart, channelEnd, offsetInStart } = fieldChannelInfo(fields, i);

        const touchesClear = channelStart > 2 || channelEnd > 2;   // channel 3 = I (clear/intensity)
        if (touchesClear) {
          return (
            <div key={i} className="snap-block disabled" data-colour="clear">
              {varName}: {channelStart === channelEnd ? "lands in" : "touches"} the clear/intensity
              channel — no reporter block exists for it, Python only
            </div>
          );
        }

        // A field either sits entirely in one RGB channel, or spans exactly two adjacent ones
        // (max field width is 16 bits = one channel, so it can never overlap more than 2). For a
        // 2-channel span, rebuild the combined 32-bit value from both reporter blocks first —
        // low channel + high channel × 65536 — then unpack from that exactly like a single
        // channel, using the combined value in place of one reporter.
        const reporter = channelStart === channelEnd
          ? <ReporterBlock label={RGBI_REPORTER[channelStart]!} />
          : <OpBlock
              a={<ReporterBlock label={RGBI_REPORTER[channelStart]!} />}
              op="+"
              b={<OpBlock a={<ReporterBlock label={RGBI_REPORTER[channelEnd]!} />} op="×" b={65536} />}
            />;

        const scale = f.scale === 0 ? 1 : f.scale;
        let value: ReactNode;
        if (f.bits === 16 && offsetInStart === 0 && channelStart === channelEnd) {
          // Whole-channel field, nothing to unpack — read the reporter directly.
          value = reporter;
        } else {
          // Sub-field sharing its channel(s) with others: raw = floor(value / 2^off) mod 2^bits
          // (same maths as `(w >> off) & mask` in the generated Python) — but when this field
          // starts at bit 0 of its channel(s), 2^off is 1, so the ÷ / floor step is a no-op
          // (floor(x / 1) === x for the non-negative reporter values these channels return):
          // skip straight to "reporter mod 2^bits" instead of showing pointless blocks.
          value = offsetInStart === 0
            ? <OpBlock a={reporter} op="mod" b={2 ** f.bits} />
            : (
              <OpBlock
                a={<FuncBlock fn="floor" arg={<OpBlock a={reporter} op="/" b={2 ** offsetInStart} />} />}
                op="mod"
                b={2 ** f.bits}
              />
            );
        }
        // A proportionally-stretched field (scale != 1, e.g. from the "x:y" button) packs its
        // whole configured min..max range into this field's raw 0..2^bits-1 codes, so that raw
        // code is already a perfectly usable number on its own (e.g. 3 bits → 0-7, low end = min,
        // high end = max) without a kid needing to reconstruct the real-world unit at all — so by
        // default (showRealValue off, the kid-mode default) skip the × scale / + offset block
        // entirely and just note the raw range. Grown-up mode (showRealValue on) instead adds the
        // maths block back, same as the COLOR/REFLT branch above, so the variable holds the real
        // reconstructed value for anyone who does want it.
        const stretched = scale !== 1 || f.offset !== 0;
        if (stretched && showRealValue) {
          if (scale !== 1) value = <OpBlock a={value} op="×" b={scale} />;
          if (f.offset !== 0) value = <OpBlock a={value} op="+" b={f.offset} />;
        }

        return (
          <div key={i} className="blocks-stack" style={{ gap: 2 }}>
            <SetVarBlock name={varName} value={value} />
            {stretched && !showRealValue && (
              <span className="muted sm">
                {varName}: raw 0–{2 ** f.bits - 1} — use it directly (0 ≈ {tidy(rangeFromScale(f.scale, f.offset, f.bits, f.signed).min)},
                {" "}{2 ** f.bits - 1} ≈ {tidy(rangeFromScale(f.scale, f.offset, f.bits, f.signed).max)}); no need to convert back to real units
              </span>
            )}
          </div>
        );
      })}
      <span className="muted sm">
        A whole 16-bit field aligned to one RGB channel (no other field sharing it) reads straight
        off the reporter block — no maths needed. A smaller field starting at the bottom of its
        channel(s) only needs a <b>mod</b>; one starting partway up (sharing a channel with a
        field before it) needs the ÷ / floor / mod chain shown, from the <b>Operators</b> category,
        to pull just its bits out (the same idea as Python's <code>&gt;&gt;</code>/
        <code>&amp;</code>). A field spanning two RGB channels first combines both reporters (low +
        high × 65536) before the same maths. Add a <b>+ Filler</b> field above to pad a following
        field out to the next channel boundary if you'd rather avoid the maths entirely.
        Double-check a sub-channel field against the live data below once — the reporter block's
        exact raw range can vary between hub firmware versions.
      </span>
    </div>
  );
}

// Complete, paste-and-run program for SPIKE Prime / Robot Inventor (hub MicroPython).
// Diagnostic colour()/reflection() lines + print fragments — always included so the printed
// output shows *everything* the sensor can report (native colour id + reflected %, alongside
// whatever's actually mapped into fields) rather than only what happens to be configured. Skips
// a value that's already shown via a COLOR/REFLT-target field (colorReflectFields), so the same
// number doesn't get printed twice under two different names.
function diagnosticLines(extra: ColorReflectField[], colourCall: string, reflectCall: string): { lines: string[]; prints: string[] } {
  const lines: string[] = [];
  const prints: string[] = [];
  if (!extra.some((e) => e.target === LEGO_TARGET_COLOR)) {
    lines.push(`    native_colour = ${colourCall}`);
    prints.push('"native_colour:", native_colour');
  }
  if (!extra.some((e) => e.target === LEGO_TARGET_REFLT)) {
    lines.push(`    native_reflect = ${reflectCall}`);
    prints.push('"native_reflect:", native_reflect');
  }
  return { lines, prints };
}

function buildSpikeProgram(fields: LegoField[], sensors: Sensor[]): string {
  const extra = colorReflectFields(fields, sensors);
  const reads = extra.map((e) => {
    const call = e.target === LEGO_TARGET_COLOR ? "color_sensor.color(port.A)" : "color_sensor.reflection(port.A)";
    const scaled = e.scale === 1 && e.offset === 0 ? call : `${call} * ${e.scale} + ${e.offset}`;
    return `    ${e.name} = ${scaled}`;
  });
  const diag = diagnosticLines(extra, "color_sensor.color(port.A)", "color_sensor.reflection(port.A)");
  return [
    "# SPIKE Prime / Robot Inventor — hub MicroPython.",
    "# Plug the MultiController board into port A.",
    "import color_sensor",
    "from hub import port",
    "import time",
    "",
    ...decodeLines(fields, sensors),
    "",
    "while True:",
    "    r, g, b, i = color_sensor.rgbi(port.A)",
    "    out = decode(r, g, b, i)",
    ...reads,
    ...diag.lines,
    `    print(out${extra.map((e) => `, ${e.name}`).join("")}${diag.prints.length ? ", " + diag.prints.join(", ") : ""})`,
    "    time.sleep_ms(100)",
  ].join("\n");
}

// Complete, paste-and-run program for Pybricks (pybricks.com firmware).
function buildPybricksProgram(fields: LegoField[], sensors: Sensor[]): string {
  const extra = colorReflectFields(fields, sensors);
  const reads = extra.map((e) => {
    const call = e.target === LEGO_TARGET_COLOR ? "device.read(0)[0]" : "device.read(1)[0]";
    const scaled = e.scale === 1 && e.offset === 0 ? call : `${call} * ${e.scale} + ${e.offset}`;
    return `    ${e.name} = ${scaled}`;
  });
  const diag = diagnosticLines(extra, "device.read(0)[0]", "device.read(1)[0]");
  return [
    "# Pybricks (pybricks.com firmware).",
    "# Plug the MultiController board into port A.",
    "from pybricks.pupdevices import PUPDevice",
    "from pybricks.parameters import Port",
    "from pybricks.tools import wait",
    "",
    "device = PUPDevice(Port.A)",
    "",
    ...decodeLines(fields, sensors),
    "",
    "while True:",
    "    r, g, b, i = device.read(5)   # mode 5 = RGB I",
    "    out = decode(r, g, b, i)",
    ...reads,
    ...diag.lines,
    `    print(out${extra.map((e) => `, ${e.name}`).join("")}${diag.prints.length ? ", " + diag.prints.join(", ") : ""})`,
    "    wait(100)",
  ].join("\n");
}

export function LegoConfigForm(p: Props) {
  const l = p.lego;
  const set = (patch: Partial<LegoConfig>) => p.onChange({ ...l, ...patch });
  const isMatrix = l.profile === LEGO_PROFILE_MATRIX;

  const updateField = (idx: number, patch: Partial<LegoField>) =>
    set({ fields: l.fields.map((f, i) => (i === idx ? { ...f, ...patch } : f)) });

  const removeField = (idx: number) =>
    set({ fields: l.fields.filter((_, i) => i !== idx) });

  const used = legoBitsUsed(l.fields);
  const remaining = LEGO_TOTAL_BITS - used;
  const over = used > LEGO_TOTAL_BITS;

  const addField = () => {
    const first = p.sensors[0];
    const wanted = first ? suggestedBits(p.sensors, first.id, 0) : 16;
    const bits: LegoBits = remaining >= wanted ? wanted
      : remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
    const { scale, offset } = first ? defaultScaleOffset(p.sensors, first.id, 0, bits) : { scale: 1, offset: 0 };
    set({
      fields: [
        ...l.fields,
        {
          sensor_id: first ? first.id : 0,
          value_index: 0,
          bits,
          signed: false,
          scale,
          offset,
        },
      ],
    });
  };

  // Padding: reserves bits without reading any sensor, so a *following* field can be pushed to
  // start at the next channel boundary — the simplest way to avoid a field splitting across two
  // RGBI channels (which word blocks can only decode with extra maths — see WordBlocksView).
  // Picks the largest width that doesn't overshoot the next boundary; add another filler
  // afterwards to close the rest (e.g. a 13-bit gap = one 8-bit filler + 4-bit + 1-bit).
  const fillerGap = (16 - (used % 16)) % 16;
  const fillerBits = ([16, 8, 4, 2, 1] as LegoBits[]).find((b) => b <= fillerGap) ?? null;
  const addFiller = () => {
    if (!fillerBits || remaining < fillerBits) return;
    set({
      fields: [...l.fields, { sensor_id: FILLER_SENSOR_ID, value_index: 0, bits: fillerBits, signed: false, scale: 1, offset: 0 }],
    });
  };
  const canAddFiller = fillerBits !== null && remaining >= fillerBits;

  const canAdd = l.fields.length < LEGO_MAX_FIELDS && remaining >= 1;

  // Set a field's value range (min/max) by deriving scale/offset for its bit width — always a
  // proportional fit (scaleFromRange), never the identity shortcut (scale=1/offset=0) that
  // defaultScaleOffset uses for its own auto defaults. Identity is a deliberate, lossy
  // simplification there (see defaultScaleOffset/currentRange) — it sends the value completely
  // unscaled but, precisely because of that, remembers nothing beyond "fits inside the raw
  // capacity", so a bits/signed change afterward can only recover the field's *whole* raw range
  // (e.g. -128..127), not whatever min/max the user actually typed. scaleFromRange is fully
  // invertible (rangeFromScale always recovers the exact typed min/max back), so a hand-typed
  // range must go through it even on the rare occasion it would technically also fit as identity.
  // When the user specifies a min/max, they typically mean the LEGO output range, not the
  // field's raw bit range. So we calculate scale/offset to map the sensor's actual value range
  // into the field's raw bit range, then apply the user's min/max on top of that.
  const setRange = (idx: number, min: number, max: number) => {
    const f = l.fields[idx];
    const rgbi = fieldTarget(f) === LEGO_TARGET_RGBI;
    const bits = rgbi ? f.bits : 8;
    const signed = rgbi ? f.signed : false;

    // The typed range IS the mapping: raw 0..2^bits-1 (or the signed equivalent) spans exactly
    // [min, max]. An earlier version derived scale/offset from the *sensor's catalogue* range
    // here and dropped the min/max arguments entirely, so hand-typing a range did nothing at all
    // — and because currentRange() inverts scale/offset to redisplay it, the typed number
    // immediately snapped back to the catalogue value, looking like the input was ignored.
    // It was.
    //
    // Always via scaleFromRange, never the identity shortcut: identity remembers nothing beyond
    // "fits the raw capacity", so a later bits/signed change could only recover the field's whole
    // raw range rather than what was typed (see currentRange's notes). scaleFromRange is exactly
    // invertible, so the typed endpoints survive.
    const { scale, offset } = scaleFromRange(min, max, bits, signed);
    updateField(idx, { scale, offset });
  };
  // Reseed a field's range from its sensor value's catalogue default (or scale=1/offset=0 as-is
  // for a bitmask/native-colour value — see defaultScaleOffset()).
  const autoRange = (idx: number) => {
    const f = l.fields[idx];
    const rgbi = fieldTarget(f) === LEGO_TARGET_RGBI;
    const { scale, offset } = defaultScaleOffset(p.sensors, f.sensor_id, f.value_index, rgbi ? f.bits : 8, rgbi ? f.signed : false);
    updateField(idx, { scale, offset });
  };
  // Force step=1 (scale=1/offset=0, raw code == real value) for the field's *current* min/max —
  // unlike defaultScaleOffset's identity shortcut, which only fires when the range happens to
  // already fit, this widens the field to the narrowest available bit width that fits the range
  // instead of falling back to a proportional stretch. Only meaningful for RGBI (COLOR/REFLT are
  // fixed at 8 bits); if the range doesn't fit even the widest offered width (16 bits, or whatever
  // bitsOptionsFor caps a discrete/bitmask value at), that widest option is used as a best effort —
  // still not proportionally scaled, so values outside it clip at the raw capacity's edge.
  const identityFit = (idx: number) => {
    const f = l.fields[idx];
    if (fieldTarget(f) !== LEGO_TARGET_RGBI) return;
    const r = currentRange(p.sensors, f, f.bits, f.signed);
    const opts = bitsOptionsFor(p.sensors, f.sensor_id, f.value_index, f.bits, !!f.colour_map);
    const bits = opts.find((b) => fitsIdentity(r.min, r.max, b, f.signed)) ?? opts[opts.length - 1];
    updateField(idx, { bits, scale: 1, offset: 0 });
  };
  // Opposite of identityFit: keep the field's current bit width fixed and proportionally stretch
  // its current min/max to fill that width's whole raw range (0..2^bits-1, or the signed
  // equivalent) — e.g. 0-1300 into 8 bits becomes steps of ~5.098, into 16 bits steps of ~0.0198.
  // Useful when the range is wider than the bits you want to spend on it (unlike identityFit,
  // this never changes bits) and you'd rather trade precision for budget than clip. setRange
  // already does exactly this proportional fit when you type a min/max by hand — this just
  // re-applies it to the field's *current* range (via currentRange, so it re-derives the sensor's
  // real range rather than an inflated raw-capacity one — see currentRange's own notes) for
  // fields that are currently sitting at identity (step 1) and would otherwise stay there.
  const stretchFit = (idx: number) => {
    const f = l.fields[idx];
    const rgbi = fieldTarget(f) === LEGO_TARGET_RGBI;
    const bits = rgbi ? f.bits : 8;
    const signed = rgbi ? f.signed : false;
    const r = currentRange(p.sensors, f, bits, signed);
    const { scale, offset } = scaleFromRange(r.min, r.max, bits, signed);
    updateField(idx, { scale, offset });
  };
  // Point a field at a different sensor/value: re-suggests bits for the new value (COLOR/REFLT
  // targets stay fixed at 8 — see suggestedBits()) and re-derives scale/offset to match, so
  // switching what a field reads doesn't leave it with a stale width/range from whatever it
  // used to point at.
  const changeFieldSource = (idx: number, sensorId: number, valueIndex: number) => {
    const f = l.fields[idx];
    const bits: LegoBits = fieldTarget(f) === LEGO_TARGET_RGBI ? suggestedBits(p.sensors, sensorId, valueIndex) : 8;
    const { scale, offset } = defaultScaleOffset(p.sensors, sensorId, valueIndex, bits);
    // Identity-scaled values that go negative (quantized sticks, -7..+7) need a signed field —
    // unsigned would clamp every left/up deflection to 0.
    const s = p.sensors.find((x) => x.id === sensorId);
    const meta = s ? sensorValueMeta(s, valueIndex) : undefined;
    const signed = isDiscreteCode(p.sensors, sensorId, valueIndex) && (meta?.min ?? 0) < 0;
    updateField(idx, { sensor_id: sensorId, value_index: valueIndex, bits, scale, offset, signed });
  };
  // Turn on the code→colour map for a COLOR-target field, seeded sensibly: code 0 (released/
  // centred) → none, then the supported colours in order, wrapping if there are more codes
  // than colours. 16 entries always — the firmware table is fixed-size; unused slots = none.
  const enableColourMap = (idx: number) => {
    const f = l.fields[idx];
    const codes = colourMapCodes(p.sensors, f);
    const m = new Array(16).fill(LEGO_COLOUR_NONE);
    for (let code = 1; code < codes; code++)
      m[code] = HUB_SUPPORTED_COLOUR_IDS[(code - 1) % HUB_SUPPORTED_COLOUR_IDS.length];
    const patch: Partial<LegoField> = { colour_map: m };
    // colour_map[] is indexed by this field's own scaled raw output (0-15, see
    // lego_emit.cpp: raw < MC_LEGO_COLOUR_MAP_N ? colour_map[raw] : none) — a continuous value
    // left at its default "native"/full-range scale (e.g. reflect 0-100%, r/g/b 0-1024) would
    // send far more than 15 almost everywhere, landing outside the table and reading as "none"
    // for nearly every actual reading. Discrete/bitmask values (dpad, detected, buttons, ...)
    // already come out as small integers and don't need this; only continuous ones do.
    if (!isDiscreteCode(p.sensors, f.sensor_id, f.value_index) && !isBitmaskValue(p.sensors, f.sensor_id, f.value_index)) {
      const s = p.sensors.find((x) => x.id === f.sensor_id);
      const meta = s ? sensorValueMeta(s, f.value_index) : undefined;
      if (meta) {
        const fit = scaleFromRange(meta.min, meta.max, 4, false);
        patch.scale = fit.scale;
        patch.offset = fit.offset;
      }
    }
    // A mapped value can be MC_LEGO_COLOUR_NONE (255) regardless of how small the underlying
    // code's own range is (see suggestedBits()/bitsOptionsFor()) — bump an RGBI field up to 8
    // bits right away if it's narrower, so turning the map on doesn't leave a field that would
    // silently clamp "none" down to whatever a real mapped code happens to be. Only the field's
    // packed WIDTH changes here — the scale/offset set above (if any) still buckets the raw
    // sensor value into the 0-15 code that indexes colour_map; that's unaffected by how wide the
    // resulting looked-up value gets packed into.
    if (fieldTarget(f) === LEGO_TARGET_RGBI && f.bits < 8) patch.bits = 8;
    updateField(idx, patch);
  };

  // Change bits or signed while keeping the same *value* range mapped (e.g. a field custom-set to
  // -255..255 stays -255..255 after a bits change — only its raw resolution/layout changes) —
  // re-deriving scale/offset from that preserved range for the new width/sign. currentRange is
  // what makes this safe: for a field still at its auto-derived default it recovers the sensor's
  // real catalogued range (not the inflated raw-capacity artefact a plain scale/offset inverse
  // would give — see currentRange's notes), and for a field with a deliberately custom range
  // (typed via the min/max fields below) it recovers that custom range exactly, so it's never
  // silently reset back to the sensor's default the moment bits or signed changes.
  //
  // Prefer identity (scale=1/offset=0, step 1) over a proportional stretch whenever the preserved
  // range still fits the new width as-is — the same preference defaultScaleOffset/"auto" already
  // apply. Without this, a range that happened to be identity-scaled before the change (e.g. step
  // 1 at 8 bits) would always come back out proportionally stretched across the *entire* new raw
  // capacity instead (e.g. step 0.0625 at 16 bits) — mathematically valid (same endpoints either
  // way) but a needlessly fine, arbitrary-looking step where "auto" would have just kept step 1.
  const remapField = (idx: number, patch: Partial<LegoField>) => {
    const f = l.fields[idx];
    const r = currentRange(p.sensors, f, f.bits, f.signed);
    const next = { ...f, ...patch };
    const { scale, offset } = identityAllowed(p.sensors, f.sensor_id, f.value_index, r.min, r.max, next.bits, next.signed)
      ? { scale: 1, offset: 0 }
      : scaleFromRange(r.min, r.max, next.bits, next.signed);
    updateField(idx, { ...patch, scale, offset });
  };

  // Switch target (RGBI word <-> COLOR/REFLT byte) while keeping the same *value* range mapped.
  // COLOR/REFLT are always a fixed 8-bit unsigned byte regardless of what bits/signed happen to
  // still be stored on the field (see the `rng` calc above) — so the "current shape" to read the
  // range back out of has to match whichever target the field is *leaving*, not just f.bits/f.signed
  // (that's what plain remapField assumes). Otherwise a field's scale/offset — fit to one target's
  // raw shape — gets left on unchanged for the new target's different shape, silently corrupting
  // the mapped range (e.g. an RGBI field's -255..255 stretch reinterpreted as an 8-bit unsigned
  // COLOR byte, or vice versa).
  const changeFieldTarget = (idx: number, newTarget: number) => {
    const f = l.fields[idx];
    const oldTarget = fieldTarget(f);
    const oldBits = oldTarget === LEGO_TARGET_RGBI ? f.bits : 8;
    const oldSigned = oldTarget === LEGO_TARGET_RGBI ? f.signed : false;
    const newBits = newTarget === LEGO_TARGET_RGBI ? f.bits : 8;
    const newSigned = newTarget === LEGO_TARGET_RGBI ? f.signed : false;
    const r = currentRange(p.sensors, f, oldBits, oldSigned);
    const { scale, offset } = identityAllowed(p.sensors, f.sensor_id, f.value_index, r.min, r.max, newBits, newSigned)
      ? { scale: 1, offset: 0 }
      : scaleFromRange(r.min, r.max, newBits, newSigned);
    updateField(idx, { target: newTarget, scale, offset });
  };

  // Colour → RGBI preset: 4 fields aligned red→R, green→G, blue→B, clear→I.
  // Uses the colour sensor's raw counts (clear=0, red=1, green=2, blue=3).
  // Only touches this sensor's own r/g/b/clear slots — every other field (other sensors,
  // manually-built fields, etc.) is left untouched rather than the whole list being wiped. And
  // if one of those 4 slots already exists (e.g. re-clicking after switching to native fields
  // and back, or after hand-tweaking a slot's width below), its bits/signed/scale/offset are
  // kept as-is instead of being reset to a fresh 16-bit unsigned default — otherwise a field
  // deliberately narrowed to fit alongside other RGBI fields would silently balloon back to 16
  // bits (and potentially no longer fit) every time this preset is (re-)applied.
  const colourSensor = p.sensors.find((s) => s.type === "tcs34725" && (s.transform || "raw") === "raw");
  const applyColourPreset = () => {
    if (!colourSensor) return;
    const slots = [1, 2, 3, 0];
    const rest = l.fields.filter(
      (f) => !(f.sensor_id === colourSensor.id && fieldTarget(f) === LEGO_TARGET_RGBI && slots.includes(f.value_index))
    );
    const preset = slots.map((vi) => {
      const existing = l.fields.find(
        (f) => f.sensor_id === colourSensor.id && fieldTarget(f) === LEGO_TARGET_RGBI && f.value_index === vi
      );
      if (existing) return { ...existing, sensor_id: colourSensor.id, value_index: vi, target: LEGO_TARGET_RGBI };
      const bits: LegoBits = 16;
      const { scale, offset } = defaultScaleOffset(p.sensors, colourSensor.id, vi, bits);
      return { sensor_id: colourSensor.id, value_index: vi, bits, signed: false, scale, offset, target: LEGO_TARGET_RGBI };
    });
    set({ fields: [...rest, ...preset] });
  };

  // Colour → native fields preset: the same 5 values real colour passthrough sends — colour id
  // and reflect% as their own COLOR/REFLT bytes (readable via color()/reflection(), no rgbi()
  // needed), plus red/green/blue packed into the RGBI word — but built from ordinary fields, so
  // it works without flipping on "colour passthrough" above (e.g. if you want to mix this
  // sensor's data with other fields, which passthrough mode doesn't allow — it ignores fields
  // entirely). Needs a sensor already in "colour + reflect + RGB" mode (col_full/as_full),
  // whose 5 values are always colour,reflect,r,g,b in that order.
  const nativeColourSensor = p.sensors.find((s) => s.transform === "col_full" || s.transform === "as_full");
  const applyNativeFieldsPreset = () => {
    if (!nativeColourSensor) return;
    // Same non-destructive approach as applyColourPreset: only this sensor's own 5 slots
    // (colour/reflect/r/g/b, value_index 0-4) are touched — all other existing fields are kept
    // as-is. The r/g/b slots (2-4) reuse an existing field's bits/signed if one is already there
    // (e.g. hand-tweaked, or carried over from a prior "Colour → RGBI" click on the same sensor)
    // instead of always resetting to 16-bit unsigned.
    const slots = [
      { vi: 0, target: LEGO_TARGET_COLOR, bits: 8 as LegoBits },
      { vi: 1, target: LEGO_TARGET_REFLT, bits: 8 as LegoBits },
      { vi: 2, target: LEGO_TARGET_RGBI, bits: 16 as LegoBits },
      { vi: 3, target: LEGO_TARGET_RGBI, bits: 16 as LegoBits },
      { vi: 4, target: LEGO_TARGET_RGBI, bits: 16 as LegoBits },
    ];
    const slotVis = slots.map((s) => s.vi);
    const rest = l.fields.filter((f) => !(f.sensor_id === nativeColourSensor.id && slotVis.includes(f.value_index)));
    const preset = slots.map(({ vi, target, bits: defaultBits }) => {
      const existing = l.fields.find((f) => f.sensor_id === nativeColourSensor.id && f.value_index === vi && fieldTarget(f) === target);
      // COLOR/REFLT slots are always a fixed 8-bit byte — no bits/signed choice to preserve there.
      const bits = target === LEGO_TARGET_RGBI && existing ? existing.bits : defaultBits;
      const signed = target === LEGO_TARGET_RGBI && existing ? existing.signed : false;
      // These 5 values are all isNativeColourValue()-recognised, so this is always scale=1/
      // offset=0 — sent exactly as real passthrough would, not stretched to fill the field.
      const { scale, offset } = defaultScaleOffset(p.sensors, nativeColourSensor.id, vi, bits);
      return { sensor_id: nativeColourSensor.id, value_index: vi, bits, signed, scale, offset, target };
    });
    set({ fields: [...rest, ...preset] });
  };

  // Quick assign: set (or clear) one of the 6 slots, rebuilding the whole field list from the
  // current simple-shape assignment of *all* slots (not just the one being changed) — R/G/B/I
  // slots after the last used one are omitted entirely; any gap before that (an unused channel
  // with a later one in use) becomes a 16-bit filler so the later channel still lands correctly.
  const setQuickSlot = (slot: QuickSlot, sensorId: number | null, valueIndex: number) => {
    const assignment = new Map<QuickSlot, { sensor_id: number; value_index: number }>();
    for (const { slot: s } of QUICK_SLOTS) {
      const f = findQuickField(l.fields, s);
      if (f) assignment.set(s, { sensor_id: f.sensor_id, value_index: f.value_index });
    }
    if (sensorId === null) assignment.delete(slot); else assignment.set(slot, { sensor_id: sensorId, value_index: valueIndex });

    const mkField = (val: { sensor_id: number; value_index: number }, bits: LegoBits, target: number, signed: boolean): LegoField => {
      const { scale, offset } = defaultScaleOffset(p.sensors, val.sensor_id, val.value_index, bits);
      return { sensor_id: val.sensor_id, value_index: val.value_index, bits, signed, scale, offset, target };
    };
    const filler = (bits: LegoBits): LegoField =>
      ({ sensor_id: FILLER_SENSOR_ID, value_index: 0, bits, signed: false, scale: 1, offset: 0, target: LEGO_TARGET_RGBI });
    // Greedy binary split of the leftover bits in a channel after a value narrower than 16 bits
    // — BITS_OPTIONS are exact powers of two, so this always exhausts `bits` precisely (e.g. a
    // 4-bit colour id leaves 12 = 8 + 4). Keeps the channel fully reserved/aligned for the *next*
    // quick-assign slot even though this one's value only needed part of it.
    const fillerChunks = (bits: number): LegoBits[] => {
      const chunks: LegoBits[] = [];
      let remaining = bits;
      for (const b of [16, 8, 4, 2, 1] as LegoBits[]) {
        while (remaining >= b) { chunks.push(b); remaining -= b; }
      }
      return chunks;
    };

    const channels = [0, 1, 2, 3] as const;
    const lastUsed = channels.reduce((last, ch) => (assignment.has(ch) ? ch : last), -1);
    const fields: LegoField[] = [];
    for (const ch of channels) {
      if (ch > lastUsed) break;
      const val = assignment.get(ch);
      if (!val) { fields.push(filler(16)); continue; }
      // Same width/signedness the detailed "+ Field" editor would suggest for this value (see
      // suggestedBits()/changeFieldSource) — Quick assign used to always force a full 16-bit
      // unsigned channel regardless of the value, which both wasted budget on narrow values
      // (e.g. a 4-bit colour id) and silently clamped negative-going ones (e.g. a quantized
      // stick axis) to 0.
      const bits = suggestedBits(p.sensors, val.sensor_id, val.value_index);
      const valSensor = p.sensors.find((s) => s.id === val.sensor_id);
      const meta = valSensor ? sensorValueMeta(valSensor, val.value_index) : undefined;
      const signed = isDiscreteCode(p.sensors, val.sensor_id, val.value_index) && (meta?.min ?? 0) < 0;
      fields.push(mkField(val, bits, LEGO_TARGET_RGBI, signed));
      for (const chunk of fillerChunks(16 - bits)) fields.push(filler(chunk));
    }
    const colorVal = assignment.get("color");
    if (colorVal) fields.push(mkField(colorVal, 8, LEGO_TARGET_COLOR, false));
    const reflectVal = assignment.get("reflect");
    if (reflectVal) fields.push(mkField(reflectVal, 8, LEGO_TARGET_REFLT, false));

    set({ fields });
  };

  const packed = packLego(l.fields, p.readings);
  const liveText = (() => {
    const rows = packed.rows.map((r, i) => {
      const s = p.sensors.find((x) => x.id === r.sensorId);
      const vn = (s ? sensorValues(s) : [])[r.valueIndex] ?? `v${r.valueIndex}`;
      const label = (r.sensorId === FILLER_SENSOR_ID ? "(filler)" : `${s ? s.name : "#" + r.sensorId}.${vn}`).padEnd(20).slice(0, 20);
      const ch = channelLabel(l.fields, i).padEnd(4);
      const f = l.fields[i];
      // With a code→value map active, the plain sensor reading (e.g. 0 for a centred stick) is
      // the *code*, not the interesting number — show its friendly label (same "none"/"N"/...
      // wording the map editor above uses) instead of a bare digit that reads like a live value.
      const valueText = f.colour_map
        ? codeLabel(vn, Math.round((r.value - f.offset) / (f.scale === 0 ? 1 : f.scale)))
        : String(tidy(r.value));
      // The hub's COLOR slot specifically translates the 0xFF/255 sentinel into color() = −1 —
      // REFLT/RGBI have no such translation and read the plain 255 back, so only COLOR's raw
      // column shows −1 here (matches the map editor's "none (sends −1)" vs "(sends 255)").
      const rawText = r.target === LEGO_TARGET_COLOR && r.raw === LEGO_COLOUR_NONE ? "-1" : String(r.raw);
      return `${ch}${label} ${valueText.padStart(10)} → ${rawText.padStart(7)}`;
    });
    const c = packed.channels;
    const extras = [
      packed.color !== null ? `COLOR=${packed.color === LEGO_COLOUR_NONE ? -1 : packed.color}` : null,
      packed.reflect !== null ? `REFLT=${packed.reflect}` : null,
    ].filter(Boolean).join("  ");
    return [
      "ch  field                     value       raw",
      ...rows,
      "",
      `hub receives:  R=${c[0]}  G=${c[1]}  B=${c[2]}  I=${c[3]}${extras ? "  " + extras : ""}`,
    ].join("\n");
  })();
  const spikeProgram = buildSpikeProgram(l.fields, p.sensors);
  const pybricksProgram = buildPybricksProgram(l.fields, p.sensors);

  return (
    <section className="card">
      <div className="card-head">
        <h2>
          LEGO sensor emitter
          <HelpTip>
            This makes your board pretend to be a real LEGO sensor — plug it into a SPIKE Prime
            or Robot Inventor hub and it'll read like magic. Try the quick presets below first!
          </HelpTip>
        </h2>
        <div className="row gap">
          <label className="check" title="Emulate a LEGO Color Sensor. (3×3 Light Matrix is disabled — still in development)">
            device
            <select
              value={l.profile}
              onChange={(e) => set({ profile: Number(e.target.value) })}
              style={{ marginLeft: 4 }}
              disabled // Only Color Sensor (0x3D) is currently enabled
            >
              <option value={LEGO_PROFILE_COLOR}>Color Sensor (0x3D)</option>
              {/* <option value={LEGO_PROFILE_MATRIX}>3×3 Light Matrix (0x40) — disabled</option> */}
            </select>
          </label>
          <label className="check">
            <input
              type="checkbox"
              checked={l.enabled}
              onChange={(e) => set({ enabled: e.target.checked })}
            />
            enabled
          </label>
        </div>
      </div>

      <div className="guide-intro" style={{ margin: "0 0 12px" }}>
        <Mascot mood={isMatrix ? "thinking" : "happy"} size={72} />
        <div>
          <h3 style={{ margin: "0 0 4px" }}>Hi, it's Brix again! 🧱</h3>
          <p className="muted sm" style={{ margin: 0, padding: 0, maxWidth: 520 }}>
            {isMatrix
              ? "This turns your board into a tiny LEGO screen — plug it into your hub, and the hub can light up its 9 little squares any colour it wants."
              : "This is the clever bit: your board can pretend to BE a real LEGO sensor! Plug it into your LEGO hub, then use \"Quick assign\" below to choose what it tells the hub."}
          </p>
        </div>
      </div>

      {p.advanced && p.board?.has_uart !== false && (
        <p className="muted sm">
          {isMatrix ? (
            <>
              Emulate a LEGO Technic 3×3 Color Light Matrix on a dedicated UART. The hub drives
              it (SPIKE matrix blocks or Pybricks <code>ColorLightMatrix</code>); incoming pixels
              render on the onboard TFT and the live grid below. Wire TX→hub RX and RX←hub TX;
              changes apply after Save.
            </>
          ) : (
            <>
              Emulate a LEGO Powered Up Color Sensor on a dedicated UART and pack selected sensor
              values into its 4×16-bit RGBI payload. A SPIKE / Pybricks program reads them back
              with <code>color_sensor.rgbi()</code> and the generated decoder below. Wire TX→hub RX
              and RX←hub TX; changes apply after Save.
            </>
          )}
        </p>
      )}

      {p.advanced ? (
        <div className="fields" style={{ marginBottom: 16 }}>
          {p.board?.has_uart !== false && (
            <>
              <NumField label="uart_port" value={l.uart_port} onChange={(v) => set({ uart_port: v })} />
              <NumField label="tx_gpio" value={l.tx_gpio} onChange={(v) => set({ tx_gpio: v })} />
              <NumField label="rx_gpio" value={l.rx_gpio} onChange={(v) => set({ rx_gpio: v })} />
              <NumField label="baud" value={l.baud} onChange={(v) => set({ baud: v })} />
            </>
          )}
          {!isMatrix && (
            <Field label="colour passthrough">
              <select value={l.colour_source} onChange={(e) => set({ colour_source: Number(e.target.value) })}>
                <option value={0}>off — bit-packing</option>
                {p.sensors
                  .filter((s) => s.transform === "col_full" || s.transform === "as_full")
                  .map((s) => (
                    <option key={s.id} value={s.id}>{s.name} (#{s.id})</option>
                  ))}
              </select>
            </Field>
          )}
        </div>
      ) : null}

      {isMatrix && <MatrixView pixels={p.matrix} />}

      {!isMatrix && (
       <>
      {l.colour_source > 0 && (
        <p className="muted sm" style={{ margin: "2px 0" }}>
          <Palette size={13} strokeWidth={2.25} className="inline-icon" /> Passthrough on — the bit-pack fields below are ignored. The hub's COLOR / REFLT /
          RGB channels (and <code>color()</code> / <code>reflection()</code> / <code>rgbi()</code>)
          come from the colour sensor. Set that sensor's convert to <code>colour + reflect + RGB</code>.
          <br />
          <AlertTriangle size={13} strokeWidth={2.25} className="inline-icon warn-icon" /> Official LEGO hub firmware (SPIKE/Robot Inventor <code>color()</code>) only recognises
          ids 0,1,3,4,6,7,9,10 — the extra classifiable colours (purple 2, cyan 5, orange 8,
          silver 11) get shown as a neighbouring colour there. Pybricks and RGBI-word fields
          read every id exactly.
        </p>
      )}

      {l.colour_source === 0 && (
        <div className="colour-section" style={{ marginBottom: 14 }}>
          <div className="colour-section-head">
            <h4 className="colour-section-title">
              Quick assign
              <HelpTip>
                A real LEGO sensor can only tell the hub 6 things: a colour, a reflect-light
                number, and four "channels" (R, G, B, I) it can pack any number into. Pick a
                sensor for each one you want to use — leave the rest as "unused".
              </HelpTip>
            </h4>
            <span className="muted sm">the 6 things a real LEGO sensor can send</span>
          </div>
          {/* Denser than a fixed 220-280px grid: the six slots are short rows, so a narrower
              minimum lets three or four sit side by side on a wide screen instead of two, and
              the compact control sizing matches the field editor below. */}
          <div className="quick-grid">
            {QUICK_SLOTS.map(({ slot, label }) => {
              const f = findQuickField(l.fields, slot);
              const sensorId = f ? f.sensor_id : QUICK_UNUSED;
              const names = f ? fieldValueNames(p.sensors, f.sensor_id) : [];
              return (
                <div className="quick-slot" key={String(slot)}>
                  <span className="quick-slot-label">{label}</span>
                  <select
                    value={sensorId}
                    style={{ flex: 1, minWidth: 0 }}
                    onChange={(e) => {
                      const id = Number(e.target.value);
                      if (id === QUICK_UNUSED) setQuickSlot(slot, null, 0);
                      else setQuickSlot(slot, id, 0);
                    }}
                  >
                    <option value={QUICK_UNUSED}>— unused —</option>
                    {p.sensors.map((s) => (
                      <option key={s.id} value={s.id}>{s.name} (#{s.id})</option>
                    ))}
                  </select>
                  {f && (
                    <select value={f.value_index} onChange={(e) => setQuickSlot(slot, f.sensor_id, Number(e.target.value))}>
                      {names.map((vn, vi) => (
                        <option key={vi} value={vi}>{vn}</option>
                      ))}
                    </select>
                  )}
                </div>
              );
            })}
          </div>
          {p.advanced && (
            <span className="muted sm">
              Only recognises a slot cleanly filled by one field — a custom sub-channel setup built
              in the detailed editor below won't show here, and assigning a slot here replaces
              whatever it currently is with the simple whole-channel/byte version.
            </span>
          )}
        </div>
      )}

      <div className="values-pick">
        <span className="recipe-label">space used</span>
        <span className={over ? "danger" : "muted"} style={{ fontSize: "0.85em" }}>
          {used} / {LEGO_TOTAL_BITS}{over ? " — too much, remove something" : ""}
        </span>
        <div
          style={{
            flex: 1,
            minWidth: 120,
            height: 6,
            background: "rgba(127,127,127,0.25)",
            borderRadius: 3,
            overflow: "hidden",
          }}
        >
          <div
            style={{
              width: `${Math.min(100, (used / LEGO_TOTAL_BITS) * 100)}%`,
              height: "100%",
              background: over ? "#e0544e" : "#4ea1e0",
            }}
          />
        </div>
        {colourSensor && (
          <button
            className="ghost sm"
            onClick={applyColourPreset}
            title={`Replace fields with red→R, green→G, blue→B, clear→I from ${colourSensor.name}`}
          >
            Colour → RGBI
          </button>
        )}
        {nativeColourSensor && (
          <button
            className="ghost sm"
            onClick={applyNativeFieldsPreset}
            title={`Replace fields with the same 5 values colour passthrough sends from ${nativeColourSensor.name}: colour→COLOR byte, reflect→REFLT byte, red/green/blue→RGBI word — but as ordinary fields, so color()/reflection()/rgbi() all work without needing passthrough mode`}
          >
            Colour → native fields
          </button>
        )}
        <button className="ghost sm" onClick={addField} disabled={!canAdd}>
          + Field
        </button>
        {p.advanced && (
          <button
            className="ghost sm"
            onClick={addFiller}
            disabled={!canAddFiller}
            title="Pad out to the next R/G/B/I channel boundary — keeps the next field you add from splitting across two channels"
          >
            + Filler ({fillerBits ?? 0} bits)
          </button>
        )}
      </div>

      {l.fields.length === 0 && (
        <p className="muted">No data selected yet — use Quick assign above, or a preset button.</p>
      )}

      {l.fields.map((f, i) => {
        const names = fieldValueNames(p.sensors, f.sensor_id);
        const target = fieldTarget(f);
        // COLOR/REFLT are always a single unsigned byte (0-255) — bits/signed don't apply, so
        // the range calc uses that fixed shape instead of the (hidden, irrelevant) field values.
        const rng = target === LEGO_TARGET_RGBI ? currentRange(p.sensors, f, f.bits, f.signed) : currentRange(p.sensors, f, 8, false);
        const step = tidy(f.scale === 0 ? 1 : f.scale);
        const chBadge = (
          <span
            style={{
              fontWeight: 700,
              padding: "2px 8px",
              borderRadius: 4,
              background: "rgba(78,161,224,0.18)",
              textAlign: "center",
              minWidth: 28,
              display: "inline-block",
            }}
          >
            {channelLabel(l.fields, i)}
          </span>
        );

        // Filler rows have no sensor/value/calibration to configure — just how many bits of
        // padding to reserve, so they get a much shorter row than a real field.
        if (isFiller(f)) {
          return (
            <div className="sensor lego-field" key={i}>
              <div className="fields">
                <Field label="ch">{chBadge}</Field>
                <span className="muted" style={{ alignSelf: "center" }}>filler — padding, always 0</span>
                <Field label="bits">
                  <select value={f.bits} onChange={(e) => updateField(i, { bits: Number(e.target.value) as LegoBits })}>
                    {BITS_OPTIONS.map((b) => (
                      <option key={b} value={b}>{b}</option>
                    ))}
                  </select>
                </Field>
                <button className="ghost sm danger" onClick={() => removeField(i)}>
                  Remove
                </button>
              </div>
            </div>
          );
        }

        return (
          <div className="sensor lego-field" key={i}>
            <div className="fields">
              <Field label="ch">{chBadge}</Field>

              <Field label="sensor">
                <select
                  value={f.sensor_id}
                  onChange={(e) => changeFieldSource(i, Number(e.target.value), 0)}
                >
                  {p.sensors.length === 0 && <option value={0}>— no sensors —</option>}
                  {p.sensors.map((s) => (
                    <option key={s.id} value={s.id}>
                      {s.name} (#{s.id})
                    </option>
                  ))}
                  {p.sensors.every((s) => s.id !== f.sensor_id) && p.sensors.length > 0 && (
                    <option value={f.sensor_id}>#{f.sensor_id} (missing)</option>
                  )}
                </select>
              </Field>

              <Field label="value">
                <select
                  value={f.value_index}
                  onChange={(e) => changeFieldSource(i, f.sensor_id, Number(e.target.value))}
                >
                  {names.length === 0 ? (
                    <option value={f.value_index}>value {f.value_index}</option>
                  ) : (
                    names.map((vn, vi) => (
                      <option key={vi} value={vi}>
                        {vn}
                      </option>
                    ))
                  )}
                </select>
              </Field>

              <Field label="target">
                <select
                  value={target}
                  title="RGBI packs this field's bits into the 64-bit word (default) — COLOR/REFLT instead drive that native byte directly, so color()/reflection() (or their word-block equivalents) see it without needing rgbi()."
                  onChange={(e) => changeFieldTarget(i, Number(e.target.value))}
                >
                  <option value={LEGO_TARGET_RGBI}>RGBI word</option>
                  <option value={LEGO_TARGET_COLOR}>COLOR byte</option>
                  <option value={LEGO_TARGET_REFLT}>REFLT byte</option>
                </select>
              </Field>

              {target === LEGO_TARGET_RGBI && (
                <>
                  <Field label="bits">
                    <select
                      value={f.bits}
                      title={`Suggested for this value: ${suggestedBits(p.sensors, f.sensor_id, f.value_index, !!f.colour_map)} bits — pick fewer to trade resolution for budget (e.g. a distance sensor rarely needs full mm precision), pick auto after to re-fit the range${f.colour_map ? ". A code→value map needs at least 8 bits — a mapped \"none\" sends 255, which a narrower field would clamp down to a real code's value." : ""}`}
                      onChange={(e) => remapField(i, { bits: Number(e.target.value) as LegoBits })}
                    >
                      {bitsOptionsFor(p.sensors, f.sensor_id, f.value_index, f.bits, !!f.colour_map).map((b) => (
                        <option key={b} value={b}>
                          {b}{b === suggestedBits(p.sensors, f.sensor_id, f.value_index, !!f.colour_map) ? " (suggested)" : ""}
                        </option>
                      ))}
                    </select>
                  </Field>

                  <label
                    className="check"
                    title="Send this value as a negative-capable (2's-complement) number — needed if you want to remap it to a range that goes below 0 (e.g. a 0-512 raw sensor mapped to -255..255), even if the sensor's own natural range never goes negative"
                  >
                    <input
                      type="checkbox"
                      checked={f.signed}
                      onChange={(e) => remapField(i, { signed: e.target.checked })}
                    />
                    signed
                  </label>
                </>
              )}

              {/* Range + the three fit actions + the resulting step, as one visual group: they
                  are a single thought ("what range, fitted how"), and grouping them is what lets
                  the row stay one line instead of the buttons drifting off on their own. */}
              <div className="lego-group">
                <label className="lego-group-label">
                  range
                  <HelpTip>
                    Specify the sensor's <b>usable value range</b>. The firmware automatically calculates scale/offset to map this range into the field's bit-width (e.g., 0-4095 sensor → 0-15 for a 4-bit field). Set min/max to the sensor values you care about, not the LEGO output range.
                  </HelpTip>
                </label>
                <NumField label="min" value={tidy(rng.min)} step={0.01} onChange={(v) => setRange(i, v, rng.max)} />
                <NumField label="max" value={tidy(rng.max)} step={0.01} onChange={(v) => setRange(i, rng.min, v)} />
                <button
                  className="ghost sm"
                  title={isBitmaskValue(p.sensors, f.sensor_id, f.value_index)
                    ? "This is a bitmask — auto sets scale=1/offset=0 (raw bits as-is) instead of a proportional range fit"
                    : isNativeColourValue(p.sensors, f.sensor_id, f.value_index)
                      ? "This is already a native colour-sensor value (0-10 / 0-100 / 0-1024) — auto sets scale=1/offset=0 to send it exactly like real passthrough would, not stretched to fill the field"
                      : "Set min/max from this value's default range"}
                  onClick={() => autoRange(i)}
                >
                  auto
                </button>
                {target === LEGO_TARGET_RGBI && (
                  <button
                    className="ghost sm"
                    title="Keep this exact min/max but force step=1 (raw code = real value, no proportional stretch) — widens the bit width to the narrowest option that fits instead of clamping"
                    onClick={() => identityFit(i)}
                  >
                    1:1
                  </button>
                )}
                <button
                  className="ghost sm"
                  title="Keep this field's current bit width but proportionally stretch min/max to fill its whole raw range (e.g. 0-1300 into 8 bits → step ≈5.098) — trades precision for budget instead of widening bits or clipping"
                  onClick={() => stretchFit(i)}
                >
                  x:y
                </button>
                <span className="step-chip" title="value change per encoder step">step {step}</span>
              </div>

              {/* Trailing actions, pushed to the right edge. output scale is a <details> rather
                  than an always-open row: it's an occasional second-stage tweak that sat at 0/0
                  on nearly every field, so as a permanent full-width block (with its own divider)
                  it roughly doubled each card's height to show two zeroes. It opens on its own
                  whenever it actually holds a value, so a configured one is never hidden. */}
              <div className="lego-actions">
                {target === LEGO_TARGET_RGBI && (
                  <details className="lego-adv" open={!!f.output_scale || !!f.output_offset}>
                    <summary title="Optional second-stage scaling — map the field's bit range onto a custom LEGO output range">
                      output scale
                    </summary>
                    <div className="lego-adv-body">
                      <label className="lego-group-label">
                        <HelpTip>
                          Optional second-stage scaling to map the field's bit range (0..2^bits-1) to a custom LEGO output range.
                          For example: 4-bit field (0-15) → piano scale (48-108) with scale=4, offset=48.
                          Leave at 0 to disable.
                        </HelpTip>
                      </label>
                      <NumField
                        label="scale"
                        value={f.output_scale ?? 0}
                        step={0.01}
                        onChange={(v) => updateField(i, { output_scale: v })}
                      />
                      <NumField
                        label="offset"
                        value={f.output_offset ?? 0}
                        step={0.01}
                        onChange={(v) => updateField(i, { output_offset: v })}
                      />
                    </div>
                  </details>
                )}
                <button className="ghost sm danger icon-only" title="Remove this field" onClick={() => removeField(i)}>
                  ×
                </button>
              </div>
            </div>
            {/* Scale warnings live under the row, not in it: inside the flex row they stretched
                it to an extra line the moment they appeared, shifting every control. */}
            {isBitmaskValue(p.sensors, f.sensor_id, f.value_index) && f.scale !== 1 && (
              <p className="muted sm lego-note" title="A bitmask field needs scale=1/offset=0 to keep individual bits intact — click auto to fix, or set them by hand">
                <AlertTriangle size={11} strokeWidth={2.25} className="inline-icon warn-icon" /> scale ≠ 1 on a bitmask — small button values may round to 0
              </p>
            )}
            {isNativeColourValue(p.sensors, f.sensor_id, f.value_index) && f.scale !== 1 && (
              <p className="muted sm lego-note" title="A native colour-sensor value needs scale=1/offset=0 to match what a real sensor sends — click auto to fix, or set them by hand">
                <AlertTriangle size={11} strokeWidth={2.25} className="inline-icon warn-icon" /> scale ≠ 1 on a native colour value — won't match real passthrough
              </p>
            )}
            {/* The COLOR/REFLT bytes are single slots on the emulated sensor: the firmware
                walks the field list and the LAST field targeting that slot wins, so extra
                fields aimed at the same byte are silently dead weight (current_color_reflt in
                lego_emit.cpp). Also flag signed/negative values on these slots — the byte is
                unsigned 0-255, so every negative reading clamps to 0. */}
            {(target === LEGO_TARGET_COLOR || target === LEGO_TARGET_REFLT) &&
              l.fields.filter((o) => fieldTarget(o) === target).length > 1 && (
              <p className="muted sm" style={{ margin: "4px 0 0", color: "var(--warn)" }}>
                <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {l.fields.filter((o) => fieldTarget(o) === target).length} fields target the{" "}
                {target === LEGO_TARGET_COLOR ? "COLOR" : "REFLT"} byte — it's a single slot, only
                the <b>last</b> one is sent; the others do nothing. Switch extras to "RGBI word" or remove them.
              </p>
            )}
            {(target === LEGO_TARGET_COLOR || target === LEGO_TARGET_REFLT) &&
              (sensorValueMeta(p.sensors.find((s) => s.id === f.sensor_id) ?? p.sensors[0], f.value_index)?.min ?? 0) < 0 && (
              <p className="muted sm" style={{ margin: "4px 0 0", color: "var(--warn)" }}>
                <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> this value goes negative but the {target === LEGO_TARGET_COLOR ? "COLOR" : "REFLT"} byte
                is unsigned 0-255 — negative readings clamp to 0. Use an RGBI-word field (signed) instead.
              </p>
            )}
            {/* The hub interprets the COLOR byte as a colour ID and validates it against the
                colour set a real sensor reports — LEGO only officially supports ids
                0,1,3,4,6,7,9,10, and unsupported ids (2/5/8/11+) get coerced to a neighbour
                (observed: sending 5 reads back as colour 6). REFLT/RGBI have no such coercion
                (plain numbers via reflection()/rgbi()); for those two the same code→value table
                is only offered for the gamepad's digital dpad/stick codes (isGamepadDigitalCode)
                — a genuinely small fixed set of states, unlike a line sensor's reflect/detected,
                a distance sensor's near/far, or a plain counter, which are continuous/derived
                readings a 16-entry map doesn't suit. COLOR stays open to any value regardless,
                since mapping to a supported colour is its whole point. Same 16-slot mechanism
                either way (see lego_emit.cpp's current_color_reflt()/current_rgbi()). */}
            {!isNativeSlotValue(p.sensors, f.sensor_id, f.value_index, target) &&
              !(names[f.value_index] ?? "").startsWith("colour") && !f.colour_map &&
              (target === LEGO_TARGET_COLOR || isGamepadDigitalCode(p.sensors, f.sensor_id, f.value_index)) && (
              <p className="muted sm" style={{ margin: "4px 0 0", color: "var(--warn)" }}>
                {target === LEGO_TARGET_COLOR
                  ? <><AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> the hub treats the COLOR byte as a colour ID and only supports 0,1,3,4,6,7,9,10 — other numbers get coerced to a nearby colour (e.g. 5 reads back as 6).</>
                  : "the hub reads this as a plain number with no colour coercion, but you can still pick a fixed value per code the same way."}{" "}
                {target !== LEGO_TARGET_COLOR && "This gamepad dpad/stick code is a small fixed set of states — "}
                <button className="ghost sm" onClick={() => enableColourMap(i)}>map codes → {target === LEGO_TARGET_COLOR ? "colours" : "values"}</button>{" "}
                to choose which {target === LEGO_TARGET_COLOR ? "supported colour" : "value"} each code/state sends, instead of the raw number.
              </p>
            )}
            {f.colour_map && target === LEGO_TARGET_RGBI && f.bits < 8 && (
              <p className="muted sm" style={{ margin: "4px 0 0", color: "var(--warn)" }}>
                <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> this field is only {f.bits} bits wide, but a mapped "none" sends 255 — it'll get
                clamped down to {2 ** f.bits - 1} on the wire, indistinguishable from a code
                deliberately mapped to that value. Set <b>bits</b> above to 8 (or wider) to fix
                this — a field created before this warning existed can still be left this narrow.
              </p>
            )}
            {f.colour_map && (
              <div className="muted sm" style={{ margin: "4px 0 0" }}>
                {target === LEGO_TARGET_COLOR
                  ? <><Palette size={12} strokeWidth={2.25} className="inline-icon" /> code → colour map (hub color() reads the chosen colour for each code;</>
                  : target === LEGO_TARGET_REFLT
                    ? <><Hash size={12} strokeWidth={2.25} className="inline-icon" /> code → value map (hub reflection() reads the chosen value for each code;</>
                    : <><Hash size={12} strokeWidth={2.25} className="inline-icon" /> code → value map (hub rgbi() reads the chosen value for each code;</>}{" "}
                <button className="ghost sm" onClick={() => updateField(i, { colour_map: undefined })}>disable</button>)
                <div style={{ display: "flex", flexWrap: "wrap", gap: 6, marginTop: 4 }}>
                  {Array.from({ length: colourMapCodes(p.sensors, f) }, (_, code) => (
                    <label key={code} style={{ display: "flex", alignItems: "center", gap: 3 }}>
                      <span>{codeLabel(names[f.value_index], code)}→</span>
                      <select
                        value={f.colour_map![code] ?? LEGO_COLOUR_NONE}
                        title={target === LEGO_TARGET_COLOR
                          ? "\"none\" and \"black (0)\" are different: none sends the wire byte 0xFF, which the hub's COLOR slot specifically converts to color() = −1 (\"no colour\"); black is a genuine classified colour, id 0, sent and read completely normally."
                          : "\"none\" sends the wire byte 0xFF (255) as a plain number — only the hub's COLOR slot re-interprets that byte as −1; REFLT/RGBI have no such conversion, so this reads back as 255."}
                        onChange={(e) => {
                          const m = [...f.colour_map!];
                          m[code] = Number(e.target.value);
                          updateField(i, { colour_map: m });
                        }}
                      >
                        {/* The wire byte is 0xFF (255) either way — but the hub's COLOR slot
                            specifically converts that to color() = −1 (its "no colour" value),
                            while REFLT/RGBI have no such conversion and just read back the plain
                            255. State whichever one the user's program will actually see. */}
                        <option value={LEGO_COLOUR_NONE}>
                          {target === LEGO_TARGET_COLOR ? "none (sends −1)" : "none (sends 255)"}
                        </option>
                        {HUB_SUPPORTED_COLOUR_IDS.map((id) => (
                          <option key={id} value={id}>
                            {target === LEGO_TARGET_COLOR
                              ? `${SPIKE_COLOURS.find((c) => c.id === id)?.name ?? `id ${id}`} (${id})`
                              : id}
                          </option>
                        ))}
                      </select>
                    </label>
                  ))}
                </div>
              </div>
            )}
            {names[f.value_index] === "buttons" && (
              <p className="muted sm" style={{ margin: "4px 0 0" }}>
                <Gamepad2 size={12} strokeWidth={2.25} className="inline-icon" /> <b>buttons</b> bit values (add together for combos; hub-side: <code>value &amp; bit</code>):
                A=1, B=2, X=4, Y=8, LB=16, RB=32, View=64, Menu=128, LS=256, RS=512,
                Xbox=1024, Share=2048, D-Up=4096, D-Down=8192, D-Left=16384, D-Right=32768
              </p>
            )}
            {(names[f.value_index] === "dpad" || names[f.value_index] === "ldir" || names[f.value_index] === "rdir") && (
              <p className="muted sm" style={{ margin: "4px 0 0" }}>
                <Gamepad2 size={12} strokeWidth={2.25} className="inline-icon" /> <b>{names[f.value_index]}</b> codes: 0={names[f.value_index] === "dpad" ? "released" : "centred (30% deadzone)"},
                {" "}1=up, 2=up-right, 3=right, 4=down-right, 5=down, 6=down-left, 7=left, 8=up-left
                — unsigned 4-bit field, scale 1
              </p>
            )}
            {/^(lx|ly|rx|ry)7$/.test(names[f.value_index] ?? "") && (
              <p className="muted sm" style={{ margin: "4px 0 0" }}>
                <Gamepad2 size={12} strokeWidth={2.25} className="inline-icon" /> quantized stick: −7 (full one way) … 0 (centred, 10% deadzone) … +7 (full the
                other) — use a <b>signed</b> 4-bit field with scale 1
              </p>
            )}
          </div>
        );
      })}

      <details className="log" open>
        <summary><Send size={13} strokeWidth={2.25} className="inline-icon" /> What's being sent to the hub right now{p.streaming ? "" : " (start streaming to update)"}</summary>
        {l.fields.length === 0 ? (
          <p className="muted sm">Nothing yet — use Quick assign above.</p>
        ) : (
          <pre>{liveText}</pre>
        )}
      </details>

      <details className="log" open>
        <summary><ToyBrick size={13} strokeWidth={2.25} className="inline-icon" /> Build it with blocks — no typing needed!</summary>
        {p.advanced && (
          <span className="muted sm">
            Grown-up mode: stretched fields show the reconstructed real value (× scale + offset). Switch back to kid mode in Settings to get the plain raw code instead.
          </span>
        )}
        <WordBlocksView lego={l} fields={l.fields} sensors={p.sensors} showRealValue={p.advanced} />
      </details>

      {p.advanced && (
        <>
          <details className="log">
            <summary><Hash size={13} strokeWidth={2.25} className="inline-icon" /> Binary maths — how the numbers get packed (for beginners)</summary>
            <BinaryMathsView fields={l.fields} sensors={p.sensors} packed={packed} />
          </details>

          <details className="log">
            <summary>Hub program — SPIKE Prime / Robot Inventor (MicroPython)</summary>
            <pre>{spikeProgram}</pre>
          </details>

          <details className="log">
            <summary>Hub program — Pybricks</summary>
            <pre>{pybricksProgram}</pre>
          </details>
        </>
      )}
       </>
      )}

      {isMatrix && (
        <details className="log" open>
          <summary>Hub program — drive the 3×3 matrix</summary>
          <pre>{matrixProgram()}</pre>
        </details>
      )}
    </section>
  );
}

// Live virtual 3×3 grid fed by the hub's pixel writes (lego_matrix BLE event). Greyed until
// the first write arrives; cells render the exact RGB the hub sent.
function MatrixView({ pixels }: { pixels: string[] | null }) {
  const cells = pixels && pixels.length === 9 ? pixels : Array(9).fill("#1b1b1b");
  return (
    <div style={{ margin: "8px 0" }}>
      <p className="muted sm" style={{ margin: "2px 0" }}>
        <Grid3x3 size={13} strokeWidth={2.25} className="inline-icon" /> Live 3×3 — pixels the hub writes appear here (and on the onboard TFT).
        {!pixels && " Waiting for the hub to set pixels…"}
      </p>
      <div
        style={{
          display: "grid",
          gridTemplateColumns: "repeat(3, 40px)",
          gridTemplateRows: "repeat(3, 40px)",
          gap: 4,
          width: "fit-content",
          padding: 6,
          background: "#000",
          borderRadius: 8,
        }}
      >
        {cells.map((c, i) => (
          <div key={i} style={{ background: c, borderRadius: 4, border: "1px solid #333" }} />
        ))}
      </div>
    </div>
  );
}

// Short hub snippet showing how to drive the emulated 3×3 matrix from either runtime.
function matrixProgram(): string {
  return [
    "# Pybricks",
    "from pybricks.pupdevices import ColorLightMatrix",
    "from pybricks.parameters import Port, Color",
    "m = ColorLightMatrix(Port.A)",
    "m.on([Color.RED, Color.GREEN, Color.BLUE] * 3)   # 9 colours, row-major",
    "",
    "# SPIKE Prime (word blocks): use the 3×3 Color Light Matrix blocks on the port",
    "# the board is wired to — they write pixels straight to this device.",
  ].join("\n");
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <label className="field">
      <span>{label}</span>
      {children}
    </label>
  );
}

function NumField({
  label,
  value,
  step,
  onChange,
}: {
  label: string;
  value: number;
  step?: number;
  onChange: (v: number) => void;
}) {
  return (
    <Field label={label}>
      <input
        type="number"
        step={step ?? 1}
        value={value}
        onFocus={(e) => e.currentTarget.select()}
        onChange={(e) => onChange(e.target.value === "" ? 0 : Number(e.target.value))}
      />
    </Field>
  );
}
