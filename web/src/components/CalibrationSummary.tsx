import { useState } from "react";
import type { Sensor } from "../types";
import { canCalibrate, isColourSensor } from "../types";
import { AlertTriangle } from "lucide-react";

// Channel labels for a sensor's calib[] array (the white-balance reference Calibrate
// captures) — matches the ColourPalette slider labels (SensorConfigForm.tsx). Other
// transforms (imu_orient/dist_*/line_reflect/ir_ball) have no fixed channel meaning here, so
// their calib values are just shown by index.
function calibLabels(s: Sensor): string[] {
  if (s.type === "as7341") return ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "Clear", "NIR"];
  if (s.type === "tcs34725") return ["clear", "red", "green", "blue"];
  return [];
}

// Channel labels for a taught colour's ref[] array. AS7341 stores the full post-whitecal
// spectrum, same 10 channels as calib — but TCS34725 stores only white-balanced+CCM-corrected
// (r,g,b) (see sensor_transform_capture_colour's TCS branch, which writes just 3 values), NOT
// the sensor's raw clear/red/green/blue counts calib holds. Using calibLabels for both here
// previously mislabelled every taught TCS colour's r/g/b as clear/red/green/blue.
export function colourRefLabels(s: Sensor): string[] {
  if (s.type === "as7341") return calibLabels(s);
  if (s.type === "tcs34725") return ["r", "g", "b"];
  return [];
}

function fmt(v: number): string {
  return Number.isFinite(v) ? String(parseFloat(v.toFixed(3))) : "—";
}

function pairs(values: number[], labels: string[]): string {
  return values.map((v, i) => `${labels[i] ?? `c${i}`}=${fmt(v)}`).join(", ");
}

// Taught refs are stored in a fixed MC_COL_CH(10)-length array regardless of sensor type, but
// TCS34725 only ever writes the first 3 (r,g,b — see sensor_transform_capture_colour); the rest
// are unused zero padding, not real data. Trim to the channels this sensor's labels cover so
// the summary doesn't pad every taught colour with a wall of meaningless "c4=0, c5=0, ...".
export function trimToLabelled(values: number[], labels: string[]): number[] {
  return labels.length > 0 ? values.slice(0, labels.length) : values;
}

// Flags taught-colour pairs that are suspiciously close in the units the classifier actually
// compares. This must mirror each sensor's real matcher or the warnings are noise:
//
// - AS7341 matches on as_dist_sq (sensor_transform.c): peak-normalised *chromaticity* over
//   F1-F8 + NIR, with Clear weighted ZERO (it holds a raw intensity count on a completely
//   different scale — thousands vs the 0-1000 F-channels) and the cyan/green/yellow bands
//   (F4-F6) up-weighted 1.6x. A plain Euclidean over all 10 channels is dominated by the
//   Clear intensity difference, which flagged chromatically-distant pairs (red vs cyan) as
//   "close" and made genuinely-confusable pairs look fine.
// - TCS34725 matches on plain Euclidean over (r,g,b) — brightness included — so that's what
//   its pairs are compared with, relative to the sensor's own median pair distance.
export interface ProximityWarning {
  a: string; b: string; dist: number; median: number;
  kind?: "chroma" | "intensity"; // as7341: which matcher the pair is actually separated by
}

// Mirror of firmware as_dist_sq: normalise each spectrum by its F1-F8 peak, ignore Clear
// (index 8), weight NIR (9) at 0.1 and F4-F6 (indices 3-5) at 1.6.
export function asChromaDistSq(a: number[], b: number[]): number {
  let aMax = 1, bMax = 1;
  for (let i = 0; i < 8; i++) {
    if ((a[i] ?? 0) > aMax) aMax = a[i];
    if ((b[i] ?? 0) > bMax) bMax = b[i];
  }
  let d = 0;
  for (let i = 0; i < 10; i++) {
    const diff = (a[i] ?? 0) / aMax - (b[i] ?? 0) / bMax;
    const w = i === 8 ? 0 : i === 9 ? 0.1 : i >= 3 && i <= 5 ? 1.6 : 1;
    d += w * diff * diff;
  }
  return d;
}

// Two AS7341 refs within this chromaticity distance of each other are genuinely at risk of
// flipping on sensor noise (the classifier accepts the nearest ref outright; scale reference:
// the untaught-defaults acceptance bound is 0.120). Empirically ~0.03 separates "two visually
// near-identical bricks" from ordinary neighbours like orange/yellow (~0.07+).
const AS_CONFUSABLE_DIST_SQ = 0.03;

// One warning line, in units that match how the pair was compared: AS7341 distances are the
// classifier's squared chromaticity (tiny decimals, fixed warn threshold in `median`); TCS
// distances are Euclidean RGB counts relative to the sensor's own median pair distance.
export function proximityText(sensor: Sensor, w: ProximityWarning): string {
  if (sensor.type === "as7341") {
    if (w.kind === "intensity")
      return `"${w.a}" and "${w.b}" have very similar brightness (raw Clear within ${Math.round((1 - w.dist) * 100)}% — neutrals are told apart by intensity alone) — re-Teach under consistent distance/LED, or expect flips`;
    return `"${w.a}" and "${w.b}" are chromatically close (distance ${w.dist.toFixed(3)}, confusable under ${w.median}) — sensor noise may flip between them`;
  }
  if (w.kind === "intensity")
    return `"${w.a}" and "${w.b}" have very similar brightness (r/g/b average within ${Math.round((1 - w.dist) * 100)}% — neutrals are told apart by brightness alone, see tcs_achromatic_match) — re-Teach under consistent distance/LED, or expect flips`;
  return `"${w.a}" and "${w.b}" are close (dist ${w.dist.toFixed(1)}, typical ${w.median.toFixed(1)}) — may be confused`;
}

export function proximityWarnings(sensor: Sensor): ProximityWarning[] {
  const taught = (sensor.colours ?? []).filter((c) => c.learned);
  if (taught.length < 2) return [];

  if (sensor.type === "as7341") {
    // Neutral pairs (black/white/silver) are separated by the *intensity* matcher — nearest
    // raw Clear count — not by chromaticity (their spectra are all flat, so a chromatic
    // "close" warning between them is meaningless). Flag those only when their raw Clears
    // are within ~15% of each other, where sensor noise could actually flip the split.
    const NEUTRAL = new Set(["black", "white", "silver", "grey", "gray"]);
    const out: ProximityWarning[] = [];
    for (let i = 0; i < taught.length; i++) {
      for (let j = i + 1; j < taught.length; j++) {
        const A = taught[i], B = taught[j];
        if (NEUTRAL.has(A.name.toLowerCase()) && NEUTRAL.has(B.name.toLowerCase())) {
          const ca = A.ref[8] ?? 0, cb = B.ref[8] ?? 0;
          const ratio = Math.min(ca, cb) / Math.max(ca, cb, 1);
          if (ratio > 0.85)
            out.push({ a: A.name, b: B.name, dist: ratio, median: 0.85, kind: "intensity" });
          continue;
        }
        const d = asChromaDistSq(A.ref, B.ref);
        if (d < AS_CONFUSABLE_DIST_SQ)
          out.push({ a: A.name, b: B.name, dist: d, median: AS_CONFUSABLE_DIST_SQ, kind: "chroma" });
      }
    }
    return out.sort((x, y) => x.dist - y.dist);
  }

  // TCS34725: neutral pairs (black/white/silver) are separated by tcs_achromatic_match's
  // brightness-only comparison (average of r,g,b) whenever the *live* sample is achromatic —
  // never by the generic chromatic RGB-distance matcher below. Comparing them by full RGB
  // distance (as every other pair is) is the wrong metric and can flag a false alarm: e.g. white
  // {250.6,255,242.11} vs silver {210.59,229.86,200.62} differ mostly in *magnitude*, which a
  // plain 3D distance conflates with genuine chromatic difference. Same fix as the AS7341 branch
  // above, same 15%-of-each-other cutoff.
  const NEUTRAL = new Set(["black", "white", "silver", "grey", "gray"]);
  const labels = colourRefLabels(sensor);
  const vectors = taught.map((c) => trimToLabelled(c.ref, labels));
  const all: ProximityWarning[] = [];
  for (let i = 0; i < taught.length; i++) {
    for (let j = i + 1; j < taught.length; j++) {
      const A = taught[i], B = taught[j];
      if (NEUTRAL.has(A.name.toLowerCase()) && NEUTRAL.has(B.name.toLowerCase())) {
        const avg = (v: number[]) => v.reduce((s, x) => s + x, 0) / (v.length || 1);
        const aAvg = avg(vectors[i]), bAvg = avg(vectors[j]);
        const ratio = Math.min(aAvg, bAvg) / Math.max(aAvg, bAvg, 1);
        if (ratio > 0.85)
          all.push({ a: A.name, b: B.name, dist: ratio, median: 0.85, kind: "intensity" });
        continue;
      }
      let sq = 0;
      for (let k = 0; k < vectors[i].length; k++) {
        const diff = (vectors[i][k] ?? 0) - (vectors[j][k] ?? 0);
        sq += diff * diff;
      }
      all.push({ a: A.name, b: B.name, dist: Math.sqrt(sq), median: 0 });
    }
  }
  if (all.length === 0) return [];

  // Intensity-flagged neutral pairs are already final (their own fixed 0.85 cutoff, like
  // AS7341's) — only the remaining chromatic pairs get the relative median-based threshold below.
  const intensityWarnings = all.filter((p) => p.kind === "intensity");
  const chromaticPairs = all.filter((p) => p.kind !== "intensity");
  if (chromaticPairs.length === 0) return intensityWarnings.sort((x, y) => x.dist - y.dist);

  const sortedDist = [...chromaticPairs].map((p) => p.dist).sort((x, y) => x - y);
  const median = sortedDist[Math.floor(sortedDist.length / 2)];
  const threshold = median * 0.5;
  return [
    ...intensityWarnings,
    ...chromaticPairs.map((p) => ({ ...p, median })).filter((p) => p.dist < threshold),
  ].sort((x, y) => x.dist - y.dist);
}

export interface NearestMatch { name: string; dist: number; threshold: number; clear: boolean; }

// Live "how far is what the sensor sees *right now* from every other taught colour" — the actual
// fix for a proximityWarnings pair: re-teaching in the exact same spot just recaptures the same
// too-close reading. This lets the physical setup (distance/angle/LED brightness) be adjusted
// *before* committing Teach, watching the number move instead of guessing and re-teaching blind.
// Same per-type metric as proximityWarnings above, so "clear" here means "would no longer be
// flagged" for real, not just a different arbitrary number.
export function nearestTaughtColour(sensor: Sensor, liveValues: number[], threshold: number, exclude: Set<string>): NearestMatch | null {
  const taught = (sensor.colours ?? []).filter((c) => c.learned && !exclude.has(c.name));
  if (taught.length === 0) return null;
  const labels = colourRefLabels(sensor);
  const live = trimToLabelled(liveValues, labels);
  if (live.length === 0) return null;
  let best: NearestMatch | null = null;
  for (const c of taught) {
    const ref = trimToLabelled(c.ref, labels);
    const dist = sensor.type === "as7341" ? asChromaDistSq(live, ref) : Math.sqrt(
      Array.from({ length: Math.max(live.length, ref.length) })
        .reduce((sq: number, _, k) => sq + ((live[k] ?? 0) - (ref[k] ?? 0)) ** 2, 0),
    );
    if (!best || dist < best.dist) best = { name: c.name, dist, threshold, clear: dist >= threshold };
  }
  return best;
}

// Saturation checks for AS7341 captures. The driver's ATIME=9/ASTEP=999 config gives an ADC
// ceiling of 10000 counts, scaled ×4 to a 40000-count full scale (drv_as7341.c runs AGAIN=64x
// and rescales to the 256x-equivalent scale) — a channel at (or within 1% of) 40000 clipped
// during capture. Taught refs are stored post-whitecal (F1-F8 peak-normalised to 1000), where
// clipping shows up as a *flat top*: several F-channels pinned at the same peak value. Both
// flatten the spectral shape the classifier matches on, which silently wrecks colour
// separation (and is the usual root cause behind a wall of proximity warnings).
const AS_RAW_CLIP = 40000 * 0.99;
export function saturationWarnings(sensor: Sensor): string[] {
  if (sensor.type !== "as7341") return [];
  const warnings: string[] = [];

  // Only F1-F8 matter here: Clear aggregates the whole spectrum and legitimately pins at full
  // scale on a bright white target (the firmware allows that, and nothing downstream uses the
  // whitecal Clear — the matcher weights it at zero), so counting it would raise a permanent
  // false alarm on perfectly good calibrations.
  const calib = sensor.calib ?? [];
  const satCalib = calib.slice(0, 8).filter((v) => v >= AS_RAW_CLIP).length;
  if (satCalib > 0)
    warnings.push(`white calibration has ${satCalib} saturated F-channel(s) (clipped at full scale 40000) — lower LED brightness / move the white target further away and re-Calibrate`);

  const NEUTRALS = ["black", "white", "silver", "grey", "gray"];
  for (const c of sensor.colours ?? []) {
    if (!c.learned) continue;
    const f = c.ref.slice(0, 8);
    if (f.some((v) => v >= 9900)) {
      warnings.push(`"${c.name}" was taught before white calibration existed (raw-scale values) — re-Teach it`);
      continue;
    }
    // Neutral colours are SUPPOSED to be flat post-whitecal — flatness only signals clipping
    // for chromatic colours, so skip the flat-top heuristic for them.
    if (!NEUTRALS.includes(c.name.toLowerCase())) {
      const peak = Math.max(...f, 1);
      const pinned = f.filter((v) => v >= peak * 0.99).length;
      if (pinned >= 4)
        warnings.push(`"${c.name}" has a flat-topped spectrum (${pinned} channels pinned at peak — likely clipped during capture) — re-Teach with lower LED/greater distance`);
    }
    // Newer firmware stores the RAW Clear count in ref[8] (intensity, for black/white/silver
    // separation) — flag it clipping, which erases exactly that separation. Two cases: at (or
    // within 1% of) the current 40000 full scale it clipped even at the driver's 64x gain;
    // pinned at ~10000 it was taught on older 256x-gain firmware whose Clear ceiling was
    // 10000 (where it clipped on almost any lit target) — either way, re-Teach.
    const rawClear = c.ref[8] ?? 0;
    if (rawClear >= AS_RAW_CLIP)
      warnings.push(`"${c.name}"'s raw Clear clipped during Teach — black/white/silver separation degraded; lower LED / add distance and re-Teach`);
    else if (rawClear >= 9900 && rawClear <= 10000)
      warnings.push(`"${c.name}" was taught on older firmware whose Clear channel clipped at 10000 — re-Teach to capture its true intensity`);
  }
  return warnings;
}

// Plain-text dump of every relevant sensor's calibration + taught colour palette — meant to be
// copied and pasted back verbatim (e.g. into a chat with whoever is helping tune the
// classifier), not just eyeballed on screen.
function asText(sensors: Sensor[]): string {
  const lines: string[] = [];
  for (const s of sensors) {
    if (!canCalibrate(s) && !isColourSensor(s.type)) continue;
    const labels = calibLabels(s);
    const hasCalib = !!s.calib && s.calib.length > 0;
    const hasColours = !!s.colours && s.colours.length > 0;
    lines.push(`${s.name} (type=${s.type}, convert=${s.transform})`);
    lines.push(hasCalib ? `  calib: ${pairs(s.calib, labels)}` : "  calib: not calibrated yet");
    if (isColourSensor(s.type)) {
      const refLabels = colourRefLabels(s);
      if (hasColours) {
        for (const c of s.colours!) {
          lines.push(`  colour "${c.name}" (out_id=${c.out_id}, ${c.learned ? "taught" : "default"}): ${c.learned ? pairs(trimToLabelled(c.ref, refLabels), refLabels) : "using built-in default"}`);
        }
      } else {
        lines.push("  colours: none taught yet");
      }
      for (const w of saturationWarnings(s)) {
        lines.push(`  ⚠ ${w}`);
      }
      for (const w of proximityWarnings(s)) {
        lines.push(`  ⚠ ${proximityText(s, w)}`);
      }
    }
    lines.push("");
  }
  return lines.join("\n").trim();
}

export function CalibrationSummary({ sensors, onReteach }: {
  sensors: Sensor[];
  // Jumps to this sensor and narrows its Colour palette section down to just the flagged
  // colours, with a live distance-to-nearest-neighbour readout per row (see
  // nearestTaughtColour) — re-teaching a too-close pair in the exact same physical spot just
  // recaptures the same collision, so the palette needs the actual warnings (not just names)
  // to know each pair's real threshold to aim for.
  onReteach?: (sensorId: number, warnings: ProximityWarning[]) => void;
}) {
  const [copied, setCopied] = useState(false);
  // Show every sensor that *can* be calibrated or is a colour sensor, whether or not it has
  // any calibration/teach data yet — so the section is always discoverable instead of
  // silently disappearing until something's been captured.
  const relevant = sensors.filter((s) => canCalibrate(s) || isColourSensor(s.type));
  if (relevant.length === 0) return null;

  const copy = () => {
    navigator.clipboard.writeText(asText(sensors)).then(() => {
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    });
  };

  return (
    <section className="card">
      <div className="card-head">
        <h2>Calibration summary</h2>
        <button className="ghost sm" onClick={copy}>{copied ? "Copied!" : "Copy for sharing"}</button>
      </div>
      <table className="grid">
        <thead>
          <tr><th>Sensor</th><th>Type / convert</th><th>Calibration</th></tr>
        </thead>
        <tbody>
          {relevant.map((s) => {
            const labels = calibLabels(s);
            const refLabels = colourRefLabels(s);
            const hasCalib = !!s.calib && s.calib.length > 0;
            const colours = s.colours ?? [];
            return (
              <tr key={s.id}>
                <td>{s.name}</td>
                <td className="muted sm">{s.type} / {s.transform}</td>
                <td style={{ fontSize: "13px" }}>
                  {hasCalib ? (
                    <div><span className="muted">calib:</span> {pairs(s.calib, labels)}</div>
                  ) : (
                    <div className="muted">not calibrated yet</div>
                  )}
                  {isColourSensor(s.type) && (
                    colours.length > 0 ? colours.map((c) => (
                      <div key={c.name}>
                        <b>{c.name}</b>{" "}
                        <span className="muted sm">(id {c.out_id}, {c.learned ? "● taught" : "○ default"})</span>
                        {c.learned ? `: ${pairs(trimToLabelled(c.ref, refLabels), refLabels)}` : ""}
                      </div>
                    )) : (
                      <div className="muted sm">no colours taught yet — using built-in defaults</div>
                    )
                  )}
                  {isColourSensor(s.type) && saturationWarnings(s).map((w, wi) => (
                    <div key={`sat-${wi}`} className="warn" style={{ marginTop: 6, padding: "6px 8px", fontSize: "12px" }}>
                      <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {w}
                    </div>
                  ))}
                  {isColourSensor(s.type) && proximityWarnings(s).map((w) => (
                    <div key={`${w.a}-${w.b}`} className="warn" style={{ marginTop: 6, padding: "6px 8px", fontSize: "12px" }}>
                      <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {proximityText(s, w)}
                    </div>
                  ))}
                  {isColourSensor(s.type) && onReteach && (() => {
                    const warnings = proximityWarnings(s);
                    if (warnings.length === 0) return null;
                    const flaggedCount = new Set(warnings.flatMap((w) => [w.a, w.b])).size;
                    return (
                      <button
                        className="ghost sm"
                        style={{ marginTop: 6 }}
                        onClick={() => onReteach(s.id, warnings)}
                        title="Jump to this sensor's Colour palette, narrowed to just these flagged colours — shows a live distance-to-nearest-neighbour readout so you can reposition before Teaching, instead of re-teaching blind in the same spot"
                      >
                        Re-teach {flaggedCount} flagged colour{flaggedCount > 1 ? "s" : ""}
                      </button>
                    );
                  })()}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </section>
  );
}
