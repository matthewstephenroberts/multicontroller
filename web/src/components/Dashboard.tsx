import { Fragment, useState } from "react";
import type { ReactNode } from "react";
import type { Reading, Sensor, SensorTimeSeries } from "../types";
import { as7341SwatchGain, rgbSwatch, sensorValueMeta, sensorValues, SPIKE_COLOURS, swatchFromRef, valueSelected } from "../types";
import { ChannelChart, timelineChannels } from "./ReadingTimeline";
import { HelpTip } from "./HelpTip";
import { Smartphone, Gamepad2, ArrowUp, ArrowDown, ArrowLeft, ArrowRight, ArrowUpLeft, ArrowUpRight, ArrowDownLeft, ArrowDownRight, ChevronUp, ChevronDown } from "lucide-react";

// Resolve a colour id (col_lego/as_lego result) to a name + swatch, preferring the sensor's
// own learned/custom palette (showing the actual taught colour), then the standard SPIKE colours.
function legoColour(s: Sensor, id: number): { name: string; swatch: string } {
  if (id < 0) return { name: "none", swatch: "transparent" };
  const entry = (s.colours ?? []).find((c) => c.out_id === id);
  const std = SPIKE_COLOURS.find((c) => c.id === id);
  const taught = entry?.learned ? swatchFromRef(s.type, entry.ref, as7341SwatchGain(s, entry.ref)) : null;
  return { name: entry?.name ?? std?.name ?? `id ${id}`, swatch: taught ?? std?.swatch ?? "#888" };
}

interface Props {
  config: Sensor[];
  readings: Record<number, Reading>;
  readingHistory: Record<number, SensorTimeSeries>;
  timelineOrder: number[];
  streaming: boolean;
  busy: string | null;
  gamepadModalOpen: boolean;
  onToggleStream: (on: boolean) => void;
  onReorder: (order: number[]) => void;
  onResetHistory: () => void;
  onToggleGamepadModal: (open: boolean) => void;
}

function ago(ts: number, now: number): string {
  const d = Math.max(0, now - ts);
  return d < 1000 ? "just now" : `${(d / 1000).toFixed(1)}s ago`;
}

// Swatch colours for the as_dist colour-match bars.
const SWATCH: Record<string, string> = {
  black: "#333", white: "#cfcfcf", red: "#e0544e", yellow: "#e0c84e",
  green: "#4ea14e", lblue: "#4ec3e0", blue: "#4e6fe0", violet: "#a14ee0",
};

// The same blue (low) → red (high) hue sweep the m5_8angle/m5_step16 firmware drivers write to
// the units' physical LEDs (angle8_value_colour/step16_position_colour) — the dashboard dials
// below deliberately match, so the on-screen colour always agrees with what the hardware shows.
function m5HueColour(v: number, max: number): string {
  const hue = 240 - (Math.max(0, Math.min(max, v)) * 240) / max;
  return `hsl(${hue}, 100%, 50%)`;
}

// One potentiometer knob as a small dial: ring + pointer in the value's hue, 270° of travel
// (7 o'clock round to 5 o'clock, like a physical pot).
function KnobDial({ v, max, label }: { v: number; max: number; label: string }) {
  const angle = -135 + (Math.max(0, Math.min(max, v)) / max) * 270;   // degrees from north
  const rad = (angle * Math.PI) / 180;
  const c = m5HueColour(v, max);
  const x2 = 20 + 11 * Math.sin(rad), y2 = 20 - 11 * Math.cos(rad);
  return (
    <div style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 1 }}
         title={`${label}: ${Math.round(v)} / ${max}`}>
      <svg width={40} height={40} viewBox="0 0 40 40">
        <circle cx={20} cy={20} r={15} fill="none" stroke="rgba(127,127,127,0.25)" strokeWidth={3} />
        <circle cx={20} cy={20} r={15} fill="none" stroke={c} strokeWidth={3}
                strokeDasharray={`${(Math.max(0, Math.min(max, v)) / max) * 0.75 * 2 * Math.PI * 15} ${2 * Math.PI * 15}`}
                strokeLinecap="round"
                transform="rotate(135 20 20)" />
        <line x1={20} y1={20} x2={x2} y2={y2} stroke={c} strokeWidth={3} strokeLinecap="round" />
      </svg>
      <span style={{ fontSize: "0.65em", opacity: 0.7 }}>{label}</span>
    </div>
  );
}

// Xbox controller visualization for gamepad sensor. `digital` selects the value range to scale
// against: "raw" transform reports full HID range (sticks ±32767, triggers 0-1023), "pad_digital"
// quantizes the *same* fields to much smaller numbers (sticks ±7, triggers 0-15) — scaling against
// the raw range regardless of which is actually configured made a digital-mode stick's dot sit
// pinned near dead centre and its trigger bars look permanently empty (a level-7 stick reads as
// 7/32768 ≈ 0.02% of full deflection), even though the values were genuinely updating.
function GamepadControllerDisplay({ reading, digital }: { reading: Reading; digital: boolean }) {
  const v = reading?.values ?? [];
  const buttons = v[0] ?? 0;
  const [lx, ly, rx, ry] = [v[1] ?? 0, v[2] ?? 0, v[3] ?? 0, v[4] ?? 0];
  const [lt, rt] = [v[5] ?? 0, v[6] ?? 0];
  const dpad = v[7] ?? 0;
  // pad_digital-only: each stick's x+y folded into one 8-way compass code (see sensor_transform.c's
  // pad_digital — same encoding as dpad: 0=centred, 1=up, clockwise to 8=up-left), values[8]/[9].
  // Undefined (and defaulting to 0) in raw mode, where these indices simply don't exist.
  const [ldir, rdir] = [v[8] ?? 0, v[9] ?? 0];

  const stickRange = digital ? 7 : 32768;
  const trigMax = digital ? 15 : 1023;

  const GAMEPAD_BUTTONS = [
    { name: "A", bit: 0 }, { name: "B", bit: 1 }, { name: "X", bit: 2 }, { name: "Y", bit: 3 },
    { name: "LB", bit: 4 }, { name: "RB", bit: 5 }, { name: "Back", bit: 6 }, { name: "Start", bit: 7 },
    { name: "L3", bit: 8 }, { name: "R3", bit: 9 },
  ];

  // Every group in this card (sticks, dirs, triggers, dpad, buttons) renders through this same
  // wrapper — label sits above its content, same size/weight/gap, same centred alignment, for
  // every group alike. Previously the sticks/triggers put their label BELOW the visual while
  // everything else put it above, so the card read as several unrelated layouts glued together.
  const Section = ({ label, children }: { label: string; children: ReactNode }) => (
    <div style={{ display: "flex", flexDirection: "column", alignItems: "center" }}>
      <div className="muted sm" style={{ height: 14, lineHeight: "14px", marginBottom: 10 }}>{label}</div>
      {children}
    </div>
  );

  // Pairs an analogue stick with its digital-direction readout (pad_digital mode only) inside
  // one bordered box, so the two stay visually grouped as "this one stick, two ways of reading
  // it" instead of reading as unrelated items that just happen to wrap next to each other.
  const Cluster = ({ children }: { children: ReactNode }) => (
    <div style={{ display: "flex", gap: 16, padding: "10px 14px", borderRadius: 8, background: "rgba(127,127,127,0.06)", border: "1px solid rgba(127,127,127,0.15)" }}>
      {children}
    </div>
  );

  const stick = (x: number, y: number) => (
    <div style={{ position: "relative", width: 48, height: 48, borderRadius: "50%", background: "rgba(127,127,127,0.15)", border: "1px solid #444" }}>
      <div
        style={{
          position: "absolute", width: 10, height: 10, borderRadius: "50%", background: "#4ea1e0",
          left: 19 + (x / stickRange) * 15, top: 19 + (y / stickRange) * 15,
        }}
      />
    </div>
  );

  const bar = (v: number, max: number, label: string) => (
    <div style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 8 }}>
      <div style={{ width: 12, height: 40, background: "rgba(127,127,127,0.15)", borderRadius: 3, display: "flex", alignItems: "flex-end", border: "1px solid #444" }}>
        <div style={{ width: "100%", height: `${Math.min(100, (v / max) * 100)}%`, background: "#4ea1e0", borderRadius: 3 }} />
      </div>
      <span className="muted sm">{label}</span>
    </div>
  );

  const dpadDir = (icon: ReactNode, label: string, code: number) => {
    const active = dpad === code;
    return (
      <span
        key={label}
        style={{
          padding: "2px 4px", borderRadius: 2, display: "flex", alignItems: "center", justifyContent: "center",
          background: active ? "#4ea1e0" : "rgba(127,127,127,0.15)",
          color: active ? "#000" : "inherit", border: "1px solid #444",
        }}
        title={label}
      >
        {icon}
      </span>
    );
  };

  // Full 8-way + centre compass for a pad_digital ldir/rdir value — unlike dpad's 4-arrow
  // display above, direction here is the WHOLE point (a folded stick with no magnitude), so all
  // 8 sectors need to be visually distinguishable, not just the 4 cardinal ones.
  const compass = (value: number) => {
    const dir = (icon: ReactNode, label: string, code: number) => {
      const active = value === code;
      return (
        <span
          key={label}
          style={{
            padding: "2px 3px", borderRadius: 2, display: "flex", alignItems: "center", justifyContent: "center",
            background: active ? "#4ea1e0" : "rgba(127,127,127,0.15)",
            color: active ? "#000" : "inherit", border: "1px solid #444",
          }}
          title={label}
        >
          {icon}
        </span>
      );
    };
    const s = 11;
    return (
      <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 18px)", gap: 2 }}>
        {dir(<ArrowUpLeft size={s} strokeWidth={2.5} />, "up-left", 8)}{dir(<ArrowUp size={s} strokeWidth={2.5} />, "up", 1)}{dir(<ArrowUpRight size={s} strokeWidth={2.5} />, "up-right", 2)}
        {dir(<ArrowLeft size={s} strokeWidth={2.5} />, "left", 7)}<span />{dir(<ArrowRight size={s} strokeWidth={2.5} />, "right", 3)}
        {dir(<ArrowDownLeft size={s} strokeWidth={2.5} />, "down-left", 6)}{dir(<ArrowDown size={s} strokeWidth={2.5} />, "down", 5)}{dir(<ArrowDownRight size={s} strokeWidth={2.5} />, "down-right", 4)}
      </div>
    );
  };

  // No "no data yet" hint here: GamepadControllerDisplay only ever renders inside ReadingCard's
  // `!stale` branch (see below), which means `reading` is already a live, actively-arriving
  // value by construction — the genuine "nothing received yet" case is ReadingCard's own
  // "waiting…" message. A resting controller (nothing touched) legitimately reports all-zero
  // values as its correct steady state — buttons up, sticks centred, dpad/ldir/rdir at 0 — so a
  // hint gated on "are all the values zero" can't tell idle-but-working apart from not-polling;
  // it fired on every single idle moment even with fresh data streaming in correctly.

  return (
    <>
      <div style={{ display: "flex", gap: 20, flexWrap: "wrap", alignItems: "flex-start", fontSize: "0.9em" }}>
        <Cluster>
          <Section label="L">{stick(lx, ly)}</Section>
          {digital && <Section label="L dir">{compass(ldir)}</Section>}
        </Cluster>
        <Cluster>
          <Section label="R">{stick(rx, ry)}</Section>
          {digital && <Section label="R dir">{compass(rdir)}</Section>}
        </Cluster>
        <Section label="trig">
          <div className="row gap">
            {bar(lt, trigMax, "LT")}
            {bar(rt, trigMax, "RT")}
          </div>
        </Section>
        <Section label="dpad">
          <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 20px)", gap: 2 }}>
            <span />{dpadDir(<ArrowUp size={12} strokeWidth={2.5} />, "up", 1)}<span />
            {dpadDir(<ArrowLeft size={12} strokeWidth={2.5} />, "left", 7)}<span />{dpadDir(<ArrowRight size={12} strokeWidth={2.5} />, "right", 3)}
            <span />{dpadDir(<ArrowDown size={12} strokeWidth={2.5} />, "down", 5)}<span />
          </div>
        </Section>
        <Section label="buttons">
          <div style={{ display: "grid", gridTemplateColumns: "repeat(5, auto)", gap: 2 }}>
            {GAMEPAD_BUTTONS.map((b) => {
              const on = (buttons & (1 << b.bit)) !== 0;
              return (
                <span
                  key={b.bit}
                  style={{
                    padding: "2px 4px", borderRadius: 2, fontSize: "0.65em", textAlign: "center", whiteSpace: "nowrap",
                    background: on ? "#4ea1e0" : "rgba(127,127,127,0.15)",
                    color: on ? "#000" : "inherit", border: "1px solid #444",
                  }}
                  title={b.name}
                >
                  {b.name}
                </span>
              );
            })}
          </div>
        </Section>
      </div>
    </>
  );
}

// Hex 0-F on a classic seven-segment layout (segments a-g), lit segments in the position's hue
// — mirrors the Step16 unit's own 7-segment position display (whose content is fixed by the
// unit's firmware; this shows the same thing, in the same colour as the unit's RGB ring).
const SEG7: Record<number, string> = {
  0: "abcdef", 1: "bc", 2: "abdeg", 3: "abcdg", 4: "bcfg", 5: "acdfg", 6: "acdefg", 7: "abc",
  8: "abcdefg", 9: "abcdfg", 10: "abcefg", 11: "cdefg", 12: "adef", 13: "bcdeg", 14: "adefg", 15: "aefg",
};
function SevenSegment({ digit, colour }: { digit: number; colour: string }) {
  const lit = SEG7[digit] ?? "";
  const seg = (name: string, x: number, y: number, w: number, h: number) => (
    <rect key={name} x={x} y={y} width={w} height={h} rx={1.5}
          fill={lit.includes(name) ? colour : "rgba(127,127,127,0.15)"} />
  );
  return (
    <svg width={26} height={44} viewBox="0 0 26 44">
      {seg("a", 5, 2, 16, 4)}
      {seg("b", 21, 5, 4, 15)}
      {seg("c", 21, 24, 4, 15)}
      {seg("d", 5, 38, 16, 4)}
      {seg("e", 1, 24, 4, 15)}
      {seg("f", 1, 5, 4, 15)}
      {seg("g", 5, 20, 16, 4)}
    </svg>
  );
}

// The Step16's rotary dial: 16 detent dots over a 300° arc, the selected one enlarged and lit
// in its hue, everything else muted.
function RotaryDial({ pos }: { pos: number }) {
  const c = m5HueColour(pos, 15);
  const dots = Array.from({ length: 16 }, (_, i) => {
    const a = ((-150 + (i * 300) / 15) * Math.PI) / 180;   // degrees from north
    const x = 26 + 20 * Math.sin(a), y = 26 - 20 * Math.cos(a);
    const sel = i === pos;
    return <circle key={i} cx={x} cy={y} r={sel ? 4 : 1.8}
                   fill={sel ? c : "rgba(127,127,127,0.4)"} />;
  });
  return (
    <svg width={52} height={52} viewBox="0 0 52 52">
      <circle cx={26} cy={26} r={12} fill="none" stroke="rgba(127,127,127,0.25)" strokeWidth={2} />
      {dots}
    </svg>
  );
}

// Composite colour swatch + per-channel intensity bars for an r/g/b value triple (col_rgb255,
// col_full, as_full) — a single number for "r" doesn't convey much, but a swatch + bars shows
// both the actual mixed colour and each channel's relative intensity at a glance.
function RgbGroup({ r, g, b, max }: { r: number; g: number; b: number; max: number }) {
  // Linear 0-255 scale for the bars — their length represents the measured value
  // proportionally, like any bar chart, so it must stay linear (no gamma).
  const scale = (x: number) => Math.max(0, Math.min(255, Math.round((x / max) * 255)));
  const sr = scale(r), sg = scale(g), sb = scale(b);
  const swatch = rgbSwatch(r, g, b, max);
  const bar = (label: string, pct: number, colour: string) => (
    <span key={label} style={{ display: "flex", alignItems: "center", gap: 4 }}>
      <span style={{ width: 8, fontSize: "0.7em", opacity: 0.7 }}>{label}</span>
      <div style={{ flex: 1, height: 6, background: "rgba(127,127,127,0.2)", borderRadius: 3, overflow: "hidden" }}>
        <div style={{ width: `${(pct / 255) * 100}%`, height: "100%", background: colour }} />
      </div>
    </span>
  );
  return (
    <span className="vv" style={{ display: "flex", flexDirection: "column", gap: 3, flex: 1 }}>
      <span style={{ display: "flex", alignItems: "center", gap: 6 }}>
        <span style={{ width: 16, height: 16, borderRadius: 3, background: swatch, border: "1px solid #555", display: "inline-block" }} />
        <span style={{ fontSize: "0.8em" }}>{Math.round(r)}, {Math.round(g)}, {Math.round(b)}</span>
      </span>
      {bar("r", sr, "#e05050")}
      {bar("g", sg, "#50c050")}
      {bar("b", sb, "#5080e0")}
    </span>
  );
}

// The live-value card for one sensor: current reading(s), status colour, bus tag. Used as the
// label-column content in the polling timeline grid, so the reading and its history sit side by
// side instead of in two separate sections.
function ReadingCard({ sensor: s, reading: r, now, onToggleGamepadModal, gamepadModalOpen }: { sensor: Sensor; reading: Reading | undefined; now: number; onToggleGamepadModal?: (open: boolean) => void; gamepadModalOpen?: boolean }) {
  // Value labels come from the *local* config's convert mode, but the values themselves are
  // shaped by whatever mode the *device* is actually running — and the two disagree whenever a
  // convert change hasn't been saved yet (or a save failed to persist). Labelling mismatched
  // data lies convincingly: a raw as7341 stream under "as_full" labels renders its F1 count as
  // "colour id 24". Only trust the mode's labels when the value count matches what that mode
  // produces; otherwise fall back to generic v0..vN and say why.
  const modeNames = sensorValues(s);
  const labelsValid = !r || modeNames.length === 0 || r.values.length === modeNames.length;
  const names = labelsValid ? modeNames : [];
  const stale = !r;
  return (
    <div className={`reading ${r?.status === "ok" ? "ok" : r ? "bad" : "idle"}`}>
      <div className="reading-head">
        <b>{s.name}</b>
        <span className={`tag ${s.type === "gamepad" ? "gamepad" : s.bus}`}>
          {s.type === "gamepad" ? "bluetooth" : s.bus}
        </span>
        {s.type === "gamepad" && onToggleGamepadModal && (
          <button className="ghost xs icon-btn" onClick={() => onToggleGamepadModal(!gamepadModalOpen)} title="Open virtual controller">
            {gamepadModalOpen ? <Smartphone size={14} strokeWidth={2.25} /> : <Gamepad2 size={14} strokeWidth={2.25} />}
          </button>
        )}
      </div>
      {stale ? (
        <div className="muted">waiting…</div>
      ) : (
        <>
          {s.type === "m5_8angle" ? (
            // Knob dials in the same hue the unit's own LEDs show (see m5HueColour) + the
            // slide switch. values: k0..k7 then switch (older firmware sends just the 8).
            <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
              <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: 2 }}>
                {r.values.slice(0, 8).map((v, i) => (
                  <KnobDial key={i} v={v} max={4095} label={names[i] ?? `k${i}`} />
                ))}
              </div>
              {r.values.length > 8 && (
                <div style={{ display: "flex", alignItems: "center", gap: 6, fontSize: "0.8em" }}>
                  <span style={{
                    width: 10, height: 10, borderRadius: "50%", display: "inline-block",
                    background: r.values[8] > 0 ? "#4ea14e" : "rgba(127,127,127,0.35)",
                  }} />
                  switch {r.values[8] > 0 ? "on" : "off"}
                </div>
              )}
            </div>
          ) : s.type === "m5_step16" ? (
            // Rotary detent dial + a 7-segment rendering of the position (0-F), both in the
            // same hue the unit's own RGB ring/digit show for this position.
            <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
              <RotaryDial pos={Math.max(0, Math.min(15, Math.round(r.values[0] ?? 0)))} />
              <SevenSegment
                digit={Math.max(0, Math.min(15, Math.round(r.values[0] ?? 0)))}
                colour={m5HueColour(r.values[0] ?? 0, 15)}
              />
              <span className="vv">{Math.round(r.values[0] ?? 0)}</span>
            </div>
          ) : s.type === "gamepad" ? (
            // Xbox controller visualization
            <GamepadControllerDisplay reading={r} digital={s.transform === "pad_digital"} />
          ) : s.transform === "as_dist" ? (
            <div style={{ display: "flex", flexDirection: "column", gap: 3 }}>
              {r.values.map((v, i) => {
                const nm = names[i] ?? `c${i}`;
                const isMax = v >= Math.max(...r.values) && v > 0;
                return (
                  <div key={i} style={{ display: "flex", alignItems: "center", gap: 6, fontWeight: isMax ? 700 : 400 }}>
                    <span style={{ width: 46, fontSize: "0.8em" }}>{nm}</span>
                    <div style={{ flex: 1, height: 10, background: "rgba(127,127,127,0.2)", borderRadius: 3, overflow: "hidden" }}>
                      <div style={{ width: `${Math.max(0, Math.min(100, v))}%`, height: "100%", background: SWATCH[nm] ?? "#888" }} />
                    </div>
                    <span style={{ width: 28, textAlign: "right", fontSize: "0.8em" }}>{v.toFixed(0)}</span>
                  </div>
                );
              })}
            </div>
          ) : (
          <div className="values">
            {(() => {
              const ri = names.indexOf("r"), gi = names.indexOf("g"), bi = names.indexOf("b");
              const rgbShown = ri >= 0 && gi >= 0 && bi >= 0 &&
                (valueSelected(s.value_mask, ri) || valueSelected(s.value_mask, gi) || valueSelected(s.value_mask, bi));
              return r.values.map((v, i) => {
                if (!valueSelected(s.value_mask, i)) return null;
                if (rgbShown && (i === gi || i === bi)) return null;   // folded into the r-index group below
                if (rgbShown && i === ri) {
                  return (
                    <div className="value" key={i}>
                      <span className="vn">rgb</span>
                      <RgbGroup r={r.values[ri]} g={r.values[gi]} b={r.values[bi]} max={sensorValueMeta(s, ri)?.max || 255} />
                    </div>
                  );
                }
                return (
                  <div className="value" key={i}>
                    <span className="vn">{names[i] ?? `v${i}`}</span>
                    {names[i] === "colour" ? (
                      (() => {
                        // Ideal taught/default reference swatch — not the live reading.
                        // The live raw r/g/b readout (with genuine sensor noise/
                        // crosstalk) belongs on its own "rgb" line below; "this is
                        // classified as X" should show what X ideally looks like.
                        const lc = legoColour(s, Math.round(v));
                        return (
                          <span className="vv" style={{ display: "flex", alignItems: "center", gap: 4 }}>
                            <span style={{ width: 12, height: 12, borderRadius: 3, background: lc.swatch, border: "1px solid #555", display: "inline-block" }} />
                            {lc.name}
                          </span>
                        );
                      })()
                    ) : (
                      <span className="vv">
                        {Number.isFinite(v) ? String(parseFloat(v.toFixed(2))) : "—"}
                        {labelsValid && sensorValueMeta(s, i)?.unit ? ` ${sensorValueMeta(s, i)!.unit}` : ""}
                      </span>
                    )}
                  </div>
                );
              });
            })()}
          </div>
          )}
          {!labelsValid && (
            <div className="muted sm" style={{ marginTop: 4 }}>
              device is streaming {r.values.length} value(s) but the selected convert mode
              expects {modeNames.length} — Save the config (or reload it) to sync, labels
              hidden until then
            </div>
          )}
          <div className="reading-foot">
            <span className="muted">{ago(r.ts, now)}</span>
            <span>{s.poll_ms} ms</span>
          </div>
        </>
      )}
    </div>
  );
}

// Coarse category used to group the live-data timeline by sensor kind (e.g. all distance
// sensors in one group, all colour sensors in another) so similar readings sit together.
type SensorCategory = "distance" | "colour" | "motion" | "environment" | "line" | "input" | "other";
const CATEGORY_LABEL: Record<SensorCategory, string> = {
  distance: "Distance", colour: "Colour", motion: "Motion", environment: "Environment",
  line: "Line / IR", input: "Input", other: "Other",
};
const CATEGORY_ORDER: SensorCategory[] = ["distance", "colour", "motion", "environment", "line", "input", "other"];
function sensorCategory(s: Sensor): SensorCategory {
  switch (s.type) {
    case "vl53l1x": case "vl53l0x": case "tof10120": case "tofi2c": return "distance";
    case "tcs34725": case "as7341": return "colour";
    case "qmi8658": case "bmi270_bmm150": return "motion";
    case "bmp280": case "bme280": case "ina226": return "environment";
    case "qre1113": case "tssp_ir": return "line";
    case "gamepad": case "gpio": case "adc": case "mcp3208": case "vk36n16": return "input";
    default: return "other";
  }
}

// Chart y-axis scaling. "auto" fits each chart to its own recorded min/max — best resolution,
// but the trace looks huge right after a reset and shrinks as the observed range grows.
// "fixed" pins each chart to its convert mode's declared value range (sensorValueMeta), so
// trace size stays stable and comparable over time; channels whose mode declares no range
// (e.g. custom recipe values) fall back to auto. Persisted so the choice survives reloads.
type ChartScale = "auto" | "fixed";
const CHART_SCALE_KEY = "mc-chart-scale";
// Per-channel deviations from the dashboard-wide mode, keyed "sensorId:valueIndex" — clicking
// a chart's scale indicator flips just that chart (e.g. a raw Clear that's unreadably flat
// against its full declared range). Cleared when the global mode changes, so the select is
// always a clean reset.
const CHART_SCALE_OVR_KEY = "mc-chart-scale-overrides";
// Layout prefs for the live-data timeline: group sensors by category (distance/colour/etc)
// and/or lay groups out across multiple columns to fit more sensors on screen at once.
const GROUP_BY_TYPE_KEY = "mc-timeline-group";
const COLUMNS_KEY = "mc-timeline-columns";

export function Dashboard(p: Props) {
  const [chartScale, setChartScale] = useState<ChartScale>(
    () => (localStorage.getItem(CHART_SCALE_KEY) === "fixed" ? "fixed" : "auto"),
  );
  const [groupByType, setGroupByType] = useState<boolean>(
    () => localStorage.getItem(GROUP_BY_TYPE_KEY) === "1",
  );
  const [columns, setColumns] = useState<number>(() => {
    const n = parseInt(localStorage.getItem(COLUMNS_KEY) ?? "1", 10);
    return n === 2 || n === 3 ? n : 1;
  });
  const changeGroupByType = (v: boolean) => {
    setGroupByType(v);
    localStorage.setItem(GROUP_BY_TYPE_KEY, v ? "1" : "0");
  };
  const changeColumns = (n: number) => {
    setColumns(n);
    localStorage.setItem(COLUMNS_KEY, String(n));
  };
  const [scaleOverrides, setScaleOverrides] = useState<Record<string, ChartScale>>(() => {
    try { return JSON.parse(localStorage.getItem(CHART_SCALE_OVR_KEY) ?? "{}"); } catch { return {}; }
  });
  const changeScale = (m: ChartScale) => {
    setChartScale(m);
    setScaleOverrides({});
    localStorage.setItem(CHART_SCALE_KEY, m);
    localStorage.removeItem(CHART_SCALE_OVR_KEY);
  };
  const toggleChannelScale = (key: string, current: ChartScale) => {
    setScaleOverrides((prev) => {
      const next = { ...prev };
      const flipped: ChartScale = current === "fixed" ? "auto" : "fixed";
      if (flipped === chartScale) delete next[key];   // back to the global mode = no override
      else next[key] = flipped;
      localStorage.setItem(CHART_SCALE_OVR_KEY, JSON.stringify(next));
      return next;
    });
  };
  const now = Date.now();
  const enabled = p.config.filter((s) => s.enabled);
  const byId = new Map(enabled.map((s) => [s.id, s]));
  const orderedIds = p.timelineOrder.filter((id) => byId.has(id));
  const ordered = orderedIds.map((id) => byId.get(id)!);

  const move = (idx: number, dir: -1 | 1) => {
    const next = [...orderedIds];
    const j = idx + dir;
    if (j < 0 || j >= next.length) return;
    [next[idx], next[j]] = [next[j], next[idx]];
    p.onReorder(next);
  };

  return (
    <>
    <section className="card">
      <div className="card-head">
        <h2>
          Live data
          <HelpTip>
            Every sensor you've turned on shows up here with a live, moving reading — this is
            where you actually watch your robot's senses working!
          </HelpTip>
        </h2>
        {ordered.length > 0 && (
          <span style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}>
            <label className="muted sm" style={{ display: "flex", alignItems: "center", gap: 4 }}>
              <input
                type="checkbox"
                checked={groupByType}
                onChange={(e) => changeGroupByType(e.target.checked)}
              />
              group by type
            </label>
            <label className="muted sm" style={{ display: "flex", alignItems: "center", gap: 4 }}>
              columns
              <select
                value={columns}
                onChange={(e) => changeColumns(parseInt(e.target.value, 10))}
                title="lay the timeline out across multiple columns to fit more sensors on screen"
              >
                <option value={1}>1</option>
                <option value={2}>2</option>
                <option value={3}>3</option>
              </select>
            </label>
            <label className="muted sm" style={{ display: "flex", alignItems: "center", gap: 4 }}>
              chart scale
              <select
                value={chartScale}
                onChange={(e) => changeScale(e.target.value as ChartScale)}
                title="auto: each chart fits its own recorded data (traces shrink as the range grows) — fixed: pin each chart to its convert mode's declared value range for a stable, comparable scale"
              >
                <option value="auto">auto (fit data)</option>
                <option value="fixed">fixed (mode range)</option>
              </select>
            </label>
            <button className="ghost sm" onClick={p.onResetHistory} title="Clear all recorded timeline data">
              Reset all data
            </button>
          </span>
        )}
      </div>

      {enabled.length === 0 ? (
        <p className="muted">Enable at least one sensor and save it to the device.</p>
      ) : (
        (() => {
          // Row renderer shared by every layout below. `idx` is this sensor's position in the
          // *global* reorder order (not its position within a group/column), so the up/down
          // buttons keep operating on the one true order regardless of how it's split visually.
          const renderRow = (s: Sensor, idx: number) => {
            const channels = timelineChannels(s);
            const ts = p.readingHistory[s.id];
            const hasData = !!ts && ts.entries.length > 0;
            const agoMs = hasData ? now - ts.lastUpdateMs : 0;
            const agoStr = agoMs < 1000 ? "just now" : `${(agoMs / 1000).toFixed(1)}s ago`;
            return (
              <Fragment key={s.id}>
                <div className="timeline-label-cell" style={{ gridRow: `span ${Math.max(1, channels.length)}` }}>
                  <div className="timeline-reorder">
                    <button className="ghost sm icon-btn" disabled={idx === 0} onClick={() => move(idx, -1)} title="Move up"><ChevronUp size={14} strokeWidth={2.5} /></button>
                    <button className="ghost sm icon-btn" disabled={idx === ordered.length - 1} onClick={() => move(idx, 1)} title="Move down"><ChevronDown size={14} strokeWidth={2.5} /></button>
                  </div>
                  <ReadingCard sensor={s} reading={p.readings[s.id]} now={now} onToggleGamepadModal={p.onToggleGamepadModal} gamepadModalOpen={p.gamepadModalOpen} />
                  {hasData && (
                    <div className="timeline-meta">
                      {ts.frequencyHz.toFixed(1)}Hz · {agoStr}
                    </div>
                  )}
                </div>
                {channels.map((c, ci) => {
                  const meta = sensorValueMeta(s, c.idx);
                  const declared = meta && Number.isFinite(meta.min) && Number.isFinite(meta.max) && meta.max > meta.min
                    ? { min: meta.min, max: meta.max }
                    : undefined;
                  const key = `${s.id}:${c.idx}`;
                  const effective: "auto" | "fixed" = scaleOverrides[key] ?? chartScale;
                  const fixed = effective === "fixed" ? declared : undefined;
                  return (
                    <div className={`timeline-chart-cell${ci === 0 ? " first" : ""}`} key={`${s.id}-${c.idx}`}>
                      <ChannelChart
                        name={c.name}
                        values={hasData ? ts.entries.map((e) => e.values[c.idx] ?? 0) : []}
                        fixed={fixed}
                        canFix={!!declared}
                        onToggleScale={declared ? () => toggleChannelScale(key, effective) : undefined}
                      />
                    </div>
                  );
                })}
              </Fragment>
            );
          };

          // Blocks are what gets distributed across columns: one labelled block per sensor
          // category when grouping is on, otherwise a single unlabelled block per sensor so
          // plain multi-column mode just spreads the flat list evenly.
          type Block = { label: string | null; sensors: Sensor[] };
          const blocks: Block[] = groupByType
            ? CATEGORY_ORDER
                .map((cat) => ({ label: CATEGORY_LABEL[cat], sensors: ordered.filter((s) => sensorCategory(s) === cat) }))
                .filter((b) => b.sensors.length > 0)
            : ordered.map((s) => ({ label: null, sensors: [s] }));

          const cols: Block[][] = Array.from({ length: columns }, () => []);
          blocks.forEach((b, i) => cols[i % columns].push(b));

          return (
            <div className="timeline-columns" style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}>
              {cols.map((blockList, ci) => (
                <div className="timeline-column" key={ci}>
                  {blockList.map((b, bi) => (
                    <div className="timeline-block" key={bi}>
                      {b.label && <h3 className="timeline-group-heading">{b.label}</h3>}
                      <div className="timeline-grid">
                        {b.sensors.map((s) => renderRow(s, orderedIds.indexOf(s.id)))}
                      </div>
                    </div>
                  ))}
                </div>
              ))}
            </div>
          );
        })()
      )}
    </section>

    </>
  );
}
