import { useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import type { BusType, Reading, Sensor, SensorActionError } from "../types";
import { as7341SwatchGain, BOARD_DA_PINS, BUS_DRIVER_TYPES, canCalibrate, defaultTcs34725Colours, DRIVER_MODES, emptyRecipe, hzLabel, isColourSensor, isDistanceSensor, MAX_GROUP_CHANNELS, NAMED_TYPES, pollMsFloor, pollMsOptions, sensorModes, sensorValues, SPIKE_COLOURS, swatchFromRef, TCS34725_PRESET_CALIB, TYPE_DESCRIPTIONS, valueSelected } from "../types";
import { CalibrationSummary, nearestTaughtColour } from "./CalibrationSummary";
import type { ProximityWarning } from "./CalibrationSummary";
import { ConfirmModal, Modal } from "./Modal";
import { HelpTip } from "./HelpTip";
import { Toggle } from "./Toggle";
import { GamepadPairingSection } from "./GamepadPairingSection";
import { AlertTriangle, Timer, Ruler, Target } from "lucide-react";

interface Props {
  config: Sensor[];
  // Open this sensor's card on arrival (the scanner's "already added" chip links here). The card
  // list shows one sensor at a time behind a type filter, so switching tabs alone would leave
  // whatever was previously selected on screen. Cleared via onFocusHandled once applied.
  focusSensorId?: number | null;
  onFocusHandled?: () => void;
  spiCsCount: number; // BOARD_SPI_CS_COUNT reported by the board — 0 = no SPI sensor slots wired at all
  hasUart?: boolean; // board capability reported by get_config — false = no aux UART wired at all
  displayEnabled: boolean;
  paged: boolean;
  // Kid/easy view (false) hides wiring-level fields (bus, addr, mux, cs_index, port, recipe) —
  // a kid tweaking values/LEDs/calibration doesn't need to see them, and Scan already fills them
  // in correctly. Grown-up/advanced view (true) shows everything, for wiring up new sensors.
  advanced: boolean;
  busy: string | null;
  readings: Record<number, Reading>;
  actionError: SensorActionError | null;
  onChange: (next: Sensor[]) => void;
  onSave: () => void;
  onCalibrate: (sensorId: number, point?: string) => void;
  onLearnColour: (sensorId: number, name: string, outId: number) => void;
  onResetColour: (sensorId: number, name: string) => void;
  onResetSensor: (sensorId: number) => void;
  onFactoryReset: () => void;
  // BLE-HID pairing state/actions for the gamepad sensor's own pairing section (rendered inline
  // in its card below — see the s.type === "gamepad" branch — instead of a separate trailing
  // card, so "pair the controller" lives right next to the sensor it drives).
  hid: { connected: boolean; name: string };
  onHidScan: () => void;
  onHidForget: () => void;
}

// line_reflect is two-point (white/black, captured separately); everything else is one-shot.
const isTwoPointCalibrate = (s: Sensor) => s.transform === "line_reflect";

// What "Calibrate" captures for the sensor's current transform — shown as help.
function calibrateHelp(s: Sensor): string {
  const t = s.transform || "";
  if (s.type === "tcs34725") return "Point at a white surface, then Calibrate — captures the white reference (clear/red/green/blue) so colours normalise correctly.";
  if (s.type === "as7341") return "Point at a white surface (LED on, teaching distance), then Calibrate — captures the 10-band white reference used by the spectral match. Works in every convert mode, including raw.";
  if (/^imu_/.test(t)) return "Hold the board completely still, then Calibrate — captures the gyro bias so orientation doesn't drift.";
  if (/^dist_/.test(t)) return "Place a target at your zero point, then Calibrate — captures the distance offset.";
  if (t === "line_reflect") return "Place the sensor over white, click Calibrate white; then over black, click Calibrate black — sets the 0.0/1.0 reflectance points.";
  if (t === "ir_ball") return "With no object in range, click Calibrate — captures the idle baseline so strength/detected read correctly.";
  return "";
}

let blankId = 1000;

// Deterministic type → LEGO colour, so the same sensor type always tabs the same colour across
// sessions/reorders instead of shifting with list position — same "recognise it by its colour"
// idea as the main tab bar, just extended to however many sensor types exist rather than the
// fixed 7 top-level tabs.
const SENSOR_TAB_COLOURS = ["red", "yellow", "blue", "green", "orange", "azure", "teal"] as const;
function colourForType(type: string): string {
  let h = 0;
  for (let i = 0; i < type.length; i++) h = (h * 31 + type.charCodeAt(i)) | 0;
  return SENSOR_TAB_COLOURS[Math.abs(h) % SENSOR_TAB_COLOURS.length];
}

export function SensorConfigForm(p: Props) {
  // Type filter tabs: with many sensors the Configure list gets long, so group the cards by
  // sensor type and let one type be shown at a time ("all" keeps the flat list). Purely a view
  // filter — cards keep their original p.config index, so every update(i) call still targets
  // the right entry regardless of the active tab.
  const [typeTab, setTypeTab] = useState<string>("all");
  // Which single sensor's card is shown — the whole point of this being a tab strip rather than
  // a stacked list is that only ONE card is ever on screen, no scrolling past every other sensor
  // to reach the one you want. Keyed by sensor id (not array index — stable across
  // add/remove/reorder, unlike an index that shifts under whatever tab currently points at it).
  const [selectedId, setSelectedId] = useState<number | null>(null);
  // Set by CalibrationSummary's "Re-teach flagged colours" button — narrows the selected
  // sensor's Colour palette down to just these colours until cleared, keeping the actual
  // warnings (not just names) so each row can show a live distance-to-nearest-neighbour
  // readout against the real threshold that flagged it — re-teaching in the exact same
  // physical spot just recaptures the same collision, so the palette needs to tell the user
  // when they've actually moved far enough before they click Teach again.
  const [focusColours, setFocusColours] = useState<{ sensorId: number; warnings: ProximityWarning[] } | null>(null);
  // Arriving from the scanner's "already added" chip: clear the type filter (the target sensor
  // may be hidden behind a different one) and select its card — the same two steps handleReteach
  // below performs for its own cross-navigation.
  useEffect(() => {
    if (p.focusSensorId == null) return;
    if (!p.config.some((s) => s.id === p.focusSensorId)) return;   // removed since the click
    setTypeTab("all");
    setSelectedId(p.focusSensorId);
    p.onFocusHandled?.();
  }, [p.focusSensorId, p.config]);

  const handleReteach = (sensorId: number, warnings: ProximityWarning[]) => {
    setTypeTab("all"); // the flagged sensor might be hidden behind a different type filter
    setSelectedId(sensorId);
    setFocusColours({ sensorId, warnings });
  };
  // In-app confirm dialogs (replacing browser confirm(), which blocks the tab and can't be
  // styled): null, or which destructive action is pending.
  const [pendingConfirm, setPendingConfirm] = useState<null | { kind: "factory" } | { kind: "reset"; sensor: Sensor } | { kind: "load_default_calib"; sensor: Sensor }>(null);
  // "Copy calibration to…": lets a sensor that's already been calibrated/taught hand that same
  // capture to other sensors of the same type+convert-mode, instead of repeating the physical
  // calibration ritual (point at white, teach each colour, ...) on every one of them. Purely a
  // local edit — same as any other Configure change, it needs Save to device afterward.
  const [copySource, setCopySource] = useState<Sensor | null>(null);
  const [copyTargets, setCopyTargets] = useState<Set<number>>(new Set());
  const typeCounts = new Map<string, number>();
  for (const s of p.config) typeCounts.set(s.type, (typeCounts.get(s.type) ?? 0) + 1);
  const typeTabs = [...typeCounts.keys()].sort();
  const activeTab = typeTab !== "all" && !typeCounts.has(typeTab) ? "all" : typeTab; // type removed → fall back
  const visible = p.config
    .map((s, i) => ({ s, i }))
    .filter(({ s }) => activeTab === "all" || s.type === activeTab);
  // Falls back to the first visible sensor whenever the current selection isn't in the filtered
  // list — nothing selected yet, the selected sensor got Removed, or a type-filter tab just
  // hid it — so there's never a dead "nothing shown" state to click through.
  const selected = visible.find(({ s }) => s.id === selectedId) ?? visible[0];

  const update = (idx: number, patch: Partial<Sensor>) =>
    p.onChange(p.config.map((s, i) => (i === idx ? { ...s, ...patch } : s)));

  const updateRecipe = (idx: number, patch: Partial<Sensor["recipe"]>) =>
    p.onChange(
      p.config.map((s, i) => (i === idx ? { ...s, recipe: { ...s.recipe, ...patch } } : s)),
    );

  const remove = (idx: number) => p.onChange(p.config.filter((_, i) => i !== idx));

  // Hide sensor types this board can't actually support — a type whose bus is always spi/uart
  // (see BUS_DRIVER_TYPES) is useless to offer when that bus has nothing wired at all, and just
  // invites picking something that'll never work. "generic" stays available regardless — it's
  // the escape hatch for any bus, not tied to one.
  const availableTypes = NAMED_TYPES.filter((t) => {
    if (t === "generic") return true;
    if (p.spiCsCount === 0 && BUS_DRIVER_TYPES.spi.includes(t)) return false;
    if (p.hasUart === false && BUS_DRIVER_TYPES.uart.includes(t)) return false;
    return true;
  });

  // Same type + same convert mode as `source` — a calibration/taught-colour capture is only
  // meaningful for another sensor reading the same physical quantity the same way (e.g. copying
  // a line_reflect white/black points onto another line_reflect sensor makes sense; copying them
  // onto one converting distance instead would not).
  const copyEligible = (source: Sensor) =>
    p.config.filter((s) => s.id !== source.id && s.type === source.type && s.transform === source.transform);

  const applyCopyCalib = () => {
    if (!copySource) return;
    const { calib, colours } = copySource;
    // colours falls back to [] (never undefined) — an explicit `colours: undefined` here would
    // get silently dropped by JSON.stringify on save, so the target's save payload would come
    // out with no "colours" key at all. Firmware treats a missing key as "keep whatever's
    // already stored for this id," so the copy would silently no-op instead of applying (or
    // clearing) the target's taught colours.
    p.onChange(
      p.config.map((s) => (copyTargets.has(s.id) ? { ...s, calib: [...calib], colours: colours ? colours.map((c) => ({ ...c })) : [] } : s)),
    );
    setCopySource(null);
  };

  const toggleValue = (idx: number, v: number) =>
    p.onChange(
      p.config.map((s, i) =>
        i === idx ? { ...s, value_mask: (s.value_mask ?? 0xffff) ^ (1 << v) } : s,
      ),
    );

  const addBlank = () =>
    p.onChange([
      ...p.config,
      {
        id: blankId++,
        name: `sensor-${blankId}`,
        type: "generic",
        bus: "i2c",
        addr: 0,
        mux_addr: 0,
        mux_channel: -1,
        cs_index: 0,
        port: 1,
        recipe: emptyRecipe(),
        transform: "raw",
        calib: [],
        poll_ms: 1000,
        enabled: true,
        simulate: false,
        show: false,
        page: 0,
        value_mask: 0xffff,
      },
    ]);

  return (
    <>
    <section className="card">
      <div className="card-head">
        <h2>
          Configure
          <HelpTip>
            This is where you tell each sensor what to measure and how often to check. Tick
            "enabled" to turn one on, then watch it come alive on the Dashboard!
          </HelpTip>
        </h2>
        <div className="row gap">
          {p.advanced && (
            <button
              className="ghost sm danger"
              disabled={!!p.busy}
              title="Erase ALL configuration (sensors, display, LEGO, colours) and restore board defaults"
              onClick={() => setPendingConfirm({ kind: "factory" })}
            >
              Factory reset
            </button>
          )}
          {p.advanced && <button className="ghost sm" title="Add an unconfigured sensor row to wire up manually — Scan is usually easier" onClick={addBlank}>+ Blank sensor</button>}
        </div>
      </div>

      {p.config.length === 0 && (
        <p className="muted">No sensors yet — scan above and add, or create a blank one.</p>
      )}

      {/* Type filter — same tab pill + glossy-stud-on-active language as the main tab bar
          (className="tab", data-colour), not a separate visual system, so "this is a selector"
          reads the same way everywhere in the app. */}
      {typeTabs.length > 1 && (
        <nav className="subtabbar" aria-label="Filter sensors by type">
          <button
            className={`tab${activeTab === "all" ? " active" : ""}`}
            data-colour="teal"
            onClick={() => setTypeTab("all")}
          >
            all ({p.config.length})
          </button>
          {typeTabs.map((t) => (
            <button
              key={t}
              className={`tab${activeTab === t ? " active" : ""}`}
              data-colour={colourForType(t)}
              onClick={() => setTypeTab(t)}
            >
              {t} ({typeCounts.get(t)})
            </button>
          ))}
        </nav>
      )}

      {/* One sensor's card at a time — the tab strip picks which, instead of every sensor in
          the current filter stacking one after another and needing a scroll to reach the Nth
          one. Same pill/stud styling as the type filter above (and the main tab bar), coloured
          per sensor *type* (colourForType) so two sensors of the same type visually match. */}
      {visible.length > 1 && (
        <nav className="subtabbar" aria-label="Choose a sensor to configure">
          {visible.map(({ s }) => (
            <button
              key={s.id}
              className={`tab${selected?.s.id === s.id ? " active" : ""}`}
              data-colour={colourForType(s.type)}
              disabled={!!p.busy}
              title={s.enabled ? undefined : "disabled"}
              style={s.enabled ? undefined : { opacity: 0.6 }}
              onClick={() => setSelectedId(s.id)}
            >
              {s.name}
            </button>
          ))}
        </nav>
      )}

      {selected && (() => { const { s, i } = selected; return (
        <>
        <div className="sensor" key={s.id}>
          <div className="sensor-top">
            <label className="check">
              <input
                type="checkbox"
                checked={s.enabled}
                onChange={(e) => update(i, { enabled: e.target.checked })}
              />
              enabled
            </label>
            <input
              className="name"
              value={s.name}
              onChange={(e) => update(i, { name: e.target.value })}
            />
            <label className="check" title="Generate plausible random data instead of reading the real bus — for testing without hardware attached">
              <input
                type="checkbox"
                checked={!!s.simulate}
                onChange={(e) => update(i, { simulate: e.target.checked })}
              />
              simulate
            </label>
            {p.displayEnabled && (
              <label className="check">
                <input
                  type="checkbox"
                  checked={s.show}
                  onChange={(e) => update(i, { show: e.target.checked })}
                />
                show
              </label>
            )}
            {p.displayEnabled && p.paged && s.show && (
              <label className="field inline">
                <span>page</span>
                <input
                  type="number"
                  value={s.page}
                  onFocus={(e) => e.currentTarget.select()}
                  onChange={(e) => update(i, { page: e.target.value === "" ? 0 : Number(e.target.value) })}
                />
              </label>
            )}
            {canCalibrate(s) && (isTwoPointCalibrate(s) ? (
              <>
                <button className="ghost sm" disabled={!!p.busy} title={calibrateHelp(s)} onClick={() => p.onCalibrate(s.id, "white")}>
                  {p.busy === "calibrating" ? "Calibrating…" : "Calibrate white"}
                </button>
                <button className="ghost sm" disabled={!!p.busy} title={calibrateHelp(s)} onClick={() => p.onCalibrate(s.id, "black")}>
                  {p.busy === "calibrating" ? "Calibrating…" : "Calibrate black"}
                </button>
              </>
            ) : (
              <button className="ghost sm" disabled={!!p.busy} title={calibrateHelp(s)} onClick={() => p.onCalibrate(s.id)}>
                {p.busy === "calibrating" ? "Calibrating…" : "Calibrate"}
              </button>
            ))}
            {s.type === "tcs34725" && (
              <button
                className="ghost sm"
                disabled={!!p.busy}
                title="Load a known-good starting calibration (white reference + all 12 colours) captured from a real unit — see docs/colour-calibration.md. Overwrites this sensor's current calibration/taught colours; re-Calibrate/re-Teach under your own conditions afterward if accuracy looks off."
                onClick={() => setPendingConfirm({ kind: "load_default_calib", sensor: s })}
              >
                Load default calibration
              </button>
            )}
            {(canCalibrate(s) || isColourSensor(s.type)) && copyEligible(s).length > 0 && (
              <button
                className="ghost sm"
                disabled={!!p.busy}
                title="Copy this sensor's calibration (and taught colours, if any) to other sensors of the same type and convert mode — no need to re-do the calibration ritual on each one"
                onClick={() => { setCopySource(s); setCopyTargets(new Set()); }}
              >
                Copy calibration to…
              </button>
            )}
            {(canCalibrate(s) || isColourSensor(s.type)) && (
              <button
                className="ghost sm danger"
                disabled={!!p.busy}
                title="Erase this sensor's captured data on the device — calibration and taught colours (type/pins/convert settings stay)"
                onClick={() => setPendingConfirm({ kind: "reset", sensor: s })}
              >
                Reset data
              </button>
            )}
            <button className="ghost sm danger" title="Remove this sensor from the config entirely — only takes effect once you Save" onClick={() => remove(i)}>Remove</button>
          </div>

          {p.actionError?.sensorId === s.id && (p.actionError.action === "calibrate" || p.actionError.action === "reset_sensor") && (
            <p className="warn" style={{ margin: "4px 0", padding: "6px 8px", fontSize: "12px" }}>
              <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {p.actionError.action === "calibrate" ? "Calibrate" : "Reset data"} rejected: {p.actionError.message}
            </p>
          )}

          {TYPE_DESCRIPTIONS[s.type] && (
            <p className="muted sm" style={{ margin: "2px 0" }}>ℹ️ {TYPE_DESCRIPTIONS[s.type]}</p>
          )}

          <div className="fields">
            <Field label="type">
              <select
                value={s.type}
                onChange={(e) => {
                  const nextType = e.target.value;
                  // tcs34725 starts pre-calibrated from a known-good real-unit capture instead
                  // of empty (see defaultTcs34725Colours()) — re-Calibrate/re-Teach under your
                  // own conditions if accuracy looks off, same as you would starting from empty.
                  update(i, {
                    type: nextType,
                    transform: DRIVER_MODES[nextType]?.[0]?.id ?? "raw",
                    calib: nextType === "tcs34725" ? [...TCS34725_PRESET_CALIB] : [],
                    colours: nextType === "tcs34725" ? defaultTcs34725Colours() : undefined,
                  });
                }}
              >
                {/* A saved sensor whose type this board can no longer support (e.g. moved
                    config to a board with no SPI/UART wired) still shows its current value —
                    never silently swap it out from under the user — just not offered as a new
                    choice, matching the pattern used for bits/poll-rate dropdowns elsewhere. */}
                {(availableTypes.includes(s.type as typeof NAMED_TYPES[number]) ? availableTypes : [...availableTypes, s.type]).map((t) => (
                  <option key={t} value={t}>{t}</option>
                ))}
              </select>
            </Field>

            <Field label="convert">
              <select value={s.transform || "raw"} onChange={(e) => update(i, { transform: e.target.value, calib: [] })}>
                {sensorModes(s).map((m) => (
                  <option key={m.id} value={m.id}>{m.label}</option>
                ))}
              </select>
            </Field>

            {/* gpio/adc = board DA pin (no bus); gamepad = virtual (no bus/pin) */}
            {s.type === "gpio" || s.type === "adc" ? (
              <>
                <Field label="pin">
                  <select value={s.port ?? BOARD_DA_PINS[0].gpio} onChange={(e) => update(i, { port: Number(e.target.value) })}>
                    {BOARD_DA_PINS.map((p) => (
                      <option key={p.gpio} value={p.gpio}>{p.label}</option>
                    ))}
                  </select>
                </Field>
                {s.type === "gpio" && (
                  <Field label="pull">
                    <select value={s.recipe.reg} onChange={(e) => updateRecipe(i, { reg: Number(e.target.value) })}>
                      <option value={0}>none</option>
                      <option value={1}>up</option>
                      <option value={2}>down</option>
                    </select>
                  </Field>
                )}
              </>
            ) : s.type === "gamepad" ? (
              <span className="muted sm">virtual sensor — no wiring, pair a controller below</span>
            ) : !p.advanced ? (
              <span className="muted sm">wiring (bus/address) hidden in kid view — switch to Grown-up mode in Settings to edit it</span>
            ) : (
              <>
                <Field label="bus">
                  <select
                    value={s.bus}
                    onChange={(e) => update(i, { bus: e.target.value as BusType })}
                  >
                    <option value="i2c">i2c</option>
                    {p.spiCsCount > 0 && <option value="spi">spi</option>}
                    {p.hasUart !== false && <option value="uart">uart</option>}
                  </select>
                </Field>

                {s.bus === "i2c" && (
                  <>
                    <NumField label="addr" value={s.addr ?? 0} onChange={(v) => update(i, { addr: v })} />
                    <NumField label="mux_addr" value={s.mux_addr ?? 0} onChange={(v) => update(i, { mux_addr: v })} />
                    <NumField label="mux_ch" value={s.mux_channel ?? -1} onChange={(v) => update(i, { mux_channel: v })} />
                  </>
                )}
                {s.bus === "spi" && p.spiCsCount > 0 && (
                  <Field label="cs_index">
                    <select
                      value={s.cs_index ?? 0}
                      onChange={(e) => update(i, { cs_index: Number(e.target.value) })}
                    >
                      {Array.from({ length: p.spiCsCount }, (_, n) => n).map((n) => (
                        <option key={n} value={n}>{n}</option>
                      ))}
                    </select>
                  </Field>
                )}
                {s.bus === "spi" && p.spiCsCount === 0 && (
                  <span className="muted sm">this board has no SPI chip-selects wired — pick a different bus</span>
                )}
                {s.bus === "spi" && s.type === "mcp3208" && (
                  <Field label="MCP3208 channel">
                    <select
                      value={s.port ?? 0}
                      onChange={(e) => update(i, { port: Number(e.target.value) })}
                    >
                      {[0, 1, 2, 3, 4, 5, 6, 7].map((n) => (
                        <option key={n} value={n}>{n}</option>
                      ))}
                    </select>
                  </Field>
                )}
                {s.bus === "spi" && (s.type === "qre1113" || s.type === "tssp_ir") && (
                  <ChannelGroupPicker sensor={s} onChange={(mask) => update(i, { channel_mask: mask })} />
                )}
                {s.bus === "uart" && p.hasUart === false && (
                  <span className="muted sm">this board has no aux UART wired — pick a different bus</span>
                )}
                {s.bus === "uart" && (
                  <NumField label="port" value={s.port ?? 1} onChange={(v) => update(i, { port: v })} />
                )}
              </>
            )}

            {(s.type === "as7341" || s.type === "m5_8angle" || s.type === "m5_step16") && (
              <Field label={`LED ${s.led ?? 0}%`}>
                <input
                  type="range"
                  min={0}
                  max={100}
                  step={5}
                  value={s.led ?? 0}
                  title={s.type === "m5_8angle"
                    ? "Per-knob RGB LED value visualisation: each knob's LED shows its position, blue = low through red = high (0 = LEDs off) — applies on Save"
                    : s.type === "m5_step16"
                      ? "All the unit's lighting: the RGB ring shows the selected step's colour (blue = 0 through red = 15) and the 7-segment digit shows the position, both at this brightness (0 = both off) — applies on Save. The digit's content is fixed by the unit itself (always the position, 0-F)."
                      : "Onboard illumination LED brightness (0 = off) — applies on Save"}
                  onChange={(e) => update(i, { led: Number(e.target.value) })}
                />
              </Field>
            )}
            {(s.type === "m5_8angle" || s.type === "m5_step16") && (s.led ?? 0) > 0 && (
              <NumField
                label="LED auto-sleep (s)"
                value={s.led_sleep_s ?? -1}
                onChange={(v) => update(i, { led_sleep_s: v < -1 ? -1 : v })}
                title="Blank the unit's LEDs after this many seconds without the knob/dial moving — movement wakes them instantly. -1 = never sleep (always lit). Same idea as the display's auto-sleep."
              />
            )}
            {s.type === "m5_8angle" && (
              <Field label="invert">
                <Toggle
                  checked={s.knob_invert ?? true}
                  onChange={(v) => update(i, { knob_invert: v })}
                  title="Flip each knob's reported value (0..4095 reversed) so turning toward the physical unit's printed min/max label reports 0/max as labelled — the pot's wiring reports it backwards by default. On by default to match the labels."
                />
              </Field>
            )}
            {s.type === "m5_8angle" && (
              <Field
                label={`smoothing ${(s.knob_smooth ?? 0) === 0 ? "off" : `${Math.round((s.knob_smooth ?? 0) * 100)}%`}`}
              >
                <input
                  type="range"
                  min={0}
                  max={0.95}
                  step={0.05}
                  value={s.knob_smooth ?? 0}
                  title="Exponential smoothing of the 8 reported knob values — a bare potentiometer wiper jitters a few counts at rest; higher = steadier but slower to react to a real turn. The onboard LED visualisation always shows the raw position immediately, unaffected by this."
                  onChange={(e) => update(i, { knob_smooth: Number(e.target.value) })}
                />
              </Field>
            )}
            {isDistanceSensor(s) && s.type === "vl53l1x" && (
              <Field label="range">
                <select
                  value={s.dist_mode ?? 0}
                  onChange={(e) => update(i, { dist_mode: Number(e.target.value) })}
                >
                  <option value={0}>short (≤1.3m, faster)</option>
                  <option value={1}>long (≤4m, slower)</option>
                </select>
              </Field>
            )}
            {isDistanceSensor(s) && (
              <>
                <NumField label="dist_min_mm" value={s.dist_min_mm ?? 0} onChange={(v) => update(i, { dist_min_mm: v })} />
                <NumField label="dist_max_mm" value={s.dist_max_mm ?? 0} onChange={(v) => update(i, { dist_max_mm: v })} />
              </>
            )}

            <Field label="poll_ms">
              <select
                value={pollMsOptions(s).includes(s.poll_ms) ? s.poll_ms : "custom"}
                onChange={(e) => e.target.value !== "custom" && update(i, { poll_ms: Number(e.target.value) })}
              >
                {!pollMsOptions(s).includes(s.poll_ms) && (
                  <option value="custom">{hzLabel(s.poll_ms)} (custom)</option>
                )}
                {pollMsOptions(s).map((ms) => (
                  <option key={ms} value={ms}>{hzLabel(ms)}{ms === pollMsFloor(s) ? " — fastest" : ""}</option>
                ))}
              </select>
            </Field>
            <NumField label="poll_ms (custom)" value={s.poll_ms} onChange={(v) => update(i, { poll_ms: v })} />
          </div>
          <p className="muted sm" style={{ margin: "2px 0" }}>
            <Timer size={12} strokeWidth={2.25} className="inline-icon" /> minimum poll interval for this sensor: {hzLabel(pollMsFloor(s))} — faster values are clamped to this on save.
          </p>

          {isDistanceSensor(s) && (
            <p className="muted sm" style={{ margin: "2px 0" }}>
              <Ruler size={12} strokeWidth={2.25} className="inline-icon" /> dist_min_mm/dist_max_mm (0 = sensor's native range) set the measuring range this
              sensor is clamped to — a LEGO field sourced from it auto-scales to that range.
              {s.type === "vl53l1x" && (s.dist_mode ? " Long range needs a longer integration time, so poll_ms is floored higher." : " Short range polls faster than long range.")}
            </p>
          )}

          {canCalibrate(s) && (
            <p className="muted sm" style={{ margin: "2px 0" }}><Target size={12} strokeWidth={2.25} className="inline-icon" /> {calibrateHelp(s)}</p>
          )}

          <div className="values-pick">
            <span className="recipe-label">monitor</span>
            {sensorValues(s).length === 0 ? (
              <span className="muted sm">no values</span>
            ) : (
              sensorValues(s).map((vn, vi) => (
                <label className="check" key={vi}>
                  <input
                    type="checkbox"
                    checked={valueSelected(s.value_mask, vi)}
                    onChange={() => toggleValue(i, vi)}
                  />
                  {vn}
                </label>
              ))
            )}
          </div>

          {s.type === "gamepad" && (
            <GamepadPairingSection
              hid={p.hid}
              busy={p.busy}
              onScan={p.onHidScan}
              onForget={p.onHidForget}
            />
          )}

          {p.advanced && (s.type === "generic" || s.type === "tofi2c") && (
            <div className="recipe">
              <span className="recipe-label">recipe</span>
              <NumField label="reg" value={s.recipe.reg} onChange={(v) => updateRecipe(i, { reg: v })} />
              <NumField label="length" value={s.recipe.length} onChange={(v) => updateRecipe(i, { length: v })} />
              <Field label="order">
                <select
                  value={s.recipe.byte_order}
                  onChange={(e) => updateRecipe(i, { byte_order: e.target.value as "be" | "le" })}
                >
                  <option value="be">be</option>
                  <option value="le">le</option>
                </select>
              </Field>
              <label className="check">
                <input
                  type="checkbox"
                  checked={s.recipe.signed}
                  onChange={(e) => updateRecipe(i, { signed: e.target.checked })}
                />
                signed
              </label>
              <NumField label="scale" value={s.recipe.scale} step={0.0001} onChange={(v) => updateRecipe(i, { scale: v })} />
              <NumField label="offset" value={s.recipe.offset} step={0.01} onChange={(v) => updateRecipe(i, { offset: v })} />
              <Field label="value_names">
                <input
                  value={s.recipe.value_names.join(",")}
                  onChange={(e) =>
                    updateRecipe(i, {
                      value_names: e.target.value.split(",").map((x) => x.trim()).filter(Boolean),
                    })
                  }
                />
              </Field>
            </div>
          )}

          {isColourSensor(s.type) && (
            <ColourPalette
              sensor={s}
              busy={p.busy}
              actionError={p.actionError?.sensorId === s.id ? p.actionError : null}
              reading={p.readings[s.id]}
              onLearn={(name, outId) => p.onLearnColour(s.id, name, outId)}
              onReset={(name) => p.onResetColour(s.id, name)}
              onUpdate={(patch) => update(i, patch)}
              focusWarnings={focusColours?.sensorId === s.id ? focusColours.warnings : undefined}
              onClearFocus={() => setFocusColours(null)}
            />
          )}
        </div>
        {/* Scoped to just this one sensor, not a page-bottom table for every sensor in the
            config — with only one sensor's card on screen at a time now (the tab strip above),
            a combined multi-sensor summary at the very bottom read as disconnected from
            whichever sensor you're actually looking at. Switching sensor tabs now switches
            this too. */}
        <CalibrationSummary sensors={[s]} onReteach={handleReteach} />
        </>
      ); })()}
    </section>

    {pendingConfirm?.kind === "factory" && (
      <ConfirmModal
        title="Factory reset"
        message="Erase ALL configuration on the device — sensors, display, LEGO settings and every taught colour — and restore board defaults?"
        confirmLabel="Erase everything"
        danger
        onConfirm={p.onFactoryReset}
        onClose={() => setPendingConfirm(null)}
      />
    )}
    {pendingConfirm?.kind === "reset" && (
      <ConfirmModal
        title={`Reset "${pendingConfirm.sensor.name}"`}
        message={`Erase this sensor's captured data on the device — its calibration and taught colours? Its type, pins and convert settings stay.`}
        confirmLabel="Reset data"
        danger
        onConfirm={() => p.onResetSensor(pendingConfirm.sensor.id)}
        onClose={() => setPendingConfirm(null)}
      />
    )}
    {pendingConfirm?.kind === "load_default_calib" && (
      <ConfirmModal
        title={`Load default calibration for "${pendingConfirm.sensor.name}"`}
        message="Replace this sensor's current white calibration and taught colours with a known-good starting point captured from a real unit? This is a real-world capture, not a universal constant — re-Calibrate/re-Teach under your own lighting afterward if accuracy looks off. This only changes the local form; Save to apply it to the device."
        confirmLabel="Load default"
        danger
        onConfirm={() => {
          const id = pendingConfirm.sensor.id;
          p.onChange(p.config.map((s) => (s.id === id ? { ...s, calib: [...TCS34725_PRESET_CALIB], colours: defaultTcs34725Colours() } : s)));
        }}
        onClose={() => setPendingConfirm(null)}
      />
    )}
    {copySource && (
      <Modal
        title={`Copy calibration from "${copySource.name}"`}
        onClose={() => setCopySource(null)}
        actions={
          <>
            <button className="ghost sm" onClick={() => setCopySource(null)}>Cancel</button>
            <button className="primary sm" disabled={copyTargets.size === 0} onClick={applyCopyCalib}>
              Copy to {copyTargets.size || ""} sensor{copyTargets.size === 1 ? "" : "s"}
            </button>
          </>
        }
      >
        <p className="muted sm" style={{ marginTop: 0 }}>
          Pick which other <b>{copySource.type}</b> sensors (same convert mode) should get this
          sensor's calibration{isColourSensor(copySource.type) ? " and taught colours" : ""} —
          this replaces whatever they currently have. Only a local edit until you{" "}
          <b>Save to device</b>.
        </p>
        {isColourSensor(copySource.type) && (
          <p className="muted sm">
            Each unit's white-point calibration and lighting differ — a target sensor mounted
            elsewhere on the robot may read a white tile with a colour cast after this copy.
            Re-run <b>Calibrate</b> (white point) on each target under its own lighting once
            copied, rather than trusting the copied values as-is.
          </p>
        )}
        {copyEligible(copySource).map((s) => (
          <label key={s.id} className="check" style={{ display: "flex", padding: "3px 0" }}>
            <input
              type="checkbox"
              checked={copyTargets.has(s.id)}
              onChange={(e) => {
                const next = new Set(copyTargets);
                if (e.target.checked) next.add(s.id); else next.delete(s.id);
                setCopyTargets(next);
              }}
            />
            {s.name}
          </label>
        ))}
      </Modal>
    )}
    </>
  );
}

// MCP3208 channel-group picker for "qre1113"/"tssp_ir": check any combination of channels 0-7
// to read them as one grouped sensor (one poll, one BLE reading, one row on the dashboard/LEGO
// field picker) instead of adding a separate sensor per channel. Falls back to displaying the
// legacy single-channel `port` as pre-checked if channel_mask hasn't been set yet — any checkbox
// interaction switches the sensor over to channel_mask going forward (see sensor.h).
function ChannelGroupPicker({ sensor, onChange }: { sensor: Sensor; onChange: (mask: number) => void }) {
  const mask = sensor.channel_mask || (1 << Math.max(0, Math.min(7, sensor.port ?? 0)));
  const twoValuesPerChannel = sensor.transform === "line_reflect" || sensor.transform === "ir_ball";
  const maxChannels = twoValuesPerChannel ? MAX_GROUP_CHANNELS : 8;
  const count = [0, 1, 2, 3, 4, 5, 6, 7].filter((c) => mask & (1 << c)).length;

  const toggle = (ch: number) => {
    const next = mask ^ (1 << ch);
    if (next !== 0) onChange(next);   // never allow an empty group
  };

  return (
    <Field label="MCP3208 channels">
      <div className="row gap" style={{ flexWrap: "wrap" }}>
        {[0, 1, 2, 3, 4, 5, 6, 7].map((ch) => {
          const checked = (mask & (1 << ch)) !== 0;
          const disabled = !checked && count >= maxChannels;
          return (
            <label key={ch} className="check" title={disabled ? `max ${maxChannels} channels for this convert mode` : undefined}>
              <input type="checkbox" checked={checked} disabled={disabled} onChange={() => toggle(ch)} />
              {ch}
            </label>
          );
        })}
      </div>
      {twoValuesPerChannel && (
        <span className="muted sm">up to {MAX_GROUP_CHANNELS} channels with this convert mode (2 values/channel, 16 max total)</span>
      )}
    </Field>
  );
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
  title,
  onChange,
}: {
  label: string;
  value: number;
  step?: number;
  title?: string;
  onChange: (v: number) => void;
}) {
  return (
    <Field label={label}>
      <input
        type="number"
        step={step ?? 1}
        value={value}
        title={title}
        onFocus={(e) => e.currentTarget.select()}
        onChange={(e) => onChange(e.target.value === "" ? 0 : Number(e.target.value))}
      />
    </Field>
  );
}

// AS7341 spectral bands in wavelength order, each bar tinted with (an sRGB approximation of)
// its band's actual colour. Clear is broadband (no wavelength) and NIR is invisible to the
// eye — shown grey/dark-red at the end.
const AS7341_BANDS: { name: string; label: string; colour: string }[] = [
  { name: "F1", label: "415nm", colour: "#7e00db" },
  { name: "F2", label: "445nm", colour: "#2300ff" },
  { name: "F3", label: "480nm", colour: "#007bff" },
  { name: "F4", label: "515nm", colour: "#00c000" },
  { name: "F5", label: "555nm", colour: "#7ed321" },
  { name: "F6", label: "590nm", colour: "#ffcf00" },
  { name: "F7", label: "630nm", colour: "#ff7300" },
  { name: "F8", label: "680nm", colour: "#ff0000" },
  { name: "Clear", label: "broad", colour: "#cfcfcf" },
  { name: "NIR", label: "910nm", colour: "#8b4a4a" },
];

// The AS7341's ADC full scale at the driver's ATIME=9/ASTEP=999 config: (9+1) × (999+1).
// A channel at (or within a whisker of) this value has CLIPPED — its true reading is higher,
// the spectrum's shape is flattened, and both white calibration and taught colours captured
// like this lose exactly the shape information the classifier matches on.
export const AS7341_FULL_SCALE = 10000;
export const as7341Saturated = (v: number) => v >= AS7341_FULL_SCALE * 0.99;

// Live 10-band spectrum bars, scaled relative to the strongest band — the shape of the
// spectrum (which the classifier matches on) stays readable regardless of gain/lighting level.
// Saturated channels are flagged loudly: teaching/calibrating while clipped silently ruins
// colour separation.
function SpectrumBars({ values }: { values: number[] }) {
  const mx = Math.max(...AS7341_BANDS.map((_, i) => values[i] ?? 0), 1);
  const satCount = AS7341_BANDS.filter((_, i) => as7341Saturated(values[i] ?? 0)).length;
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 3, maxWidth: 420 }}>
      {AS7341_BANDS.map((band, i) => {
        const v = values[i] ?? 0;
        const sat = as7341Saturated(v);
        return (
          <div key={band.name} style={{ display: "flex", alignItems: "center", gap: 8, fontSize: "12px" }}>
            <span style={{ width: 34 }}>{band.name}</span>
            <span className="muted" style={{ width: 42, fontSize: "11px" }}>{band.label}</span>
            <div style={{ flex: 1, height: 12, background: "rgba(127,127,127,0.15)", borderRadius: 3, overflow: "hidden", outline: sat ? "1px solid var(--danger)" : undefined }}>
              <div style={{ width: `${(v / mx) * 100}%`, height: "100%", background: band.colour }} />
            </div>
            <span style={{ width: 48, textAlign: "right", fontVariantNumeric: "tabular-nums" }} className={sat ? "" : "muted"}>
              {sat ? "SAT" : Math.round(v)}
            </span>
          </div>
        );
      })}
      {satCount > 0 && (
        <p className="warn" style={{ margin: "6px 0 0", padding: "6px 8px", fontSize: "12px" }}>
          <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {satCount} channel{satCount > 1 ? "s" : ""} saturated (clipped at full scale) — lower the
          LED brightness or move the target further away until no band reads SAT, then re-Calibrate
          white and re-Teach colours. Clipped captures flatten the spectrum and make colours
          indistinguishable.
        </p>
      )}
    </div>
  );
}

// Learnable colour palette editor for a colour sensor. Lists the standard SPIKE colours
// (always teachable) plus any custom colours, with per-entry Teach / Reset, an editable
// white-balance calibration (Calibrate captures it; the palette exposes the calibrated
// values directly as sliders for fine-tuning), and per-colour reference sliders for
// tweaking a taught colour after Teach — everything updates the swatches/wheel live as you drag.
function ColourPalette({
  sensor,
  busy,
  actionError,
  reading,
  onLearn,
  onReset,
  onUpdate,
  focusWarnings,
  onClearFocus,
}: {
  sensor: Sensor;
  busy: string | null;
  actionError: SensorActionError | null; // already filtered to this sensor by the parent
  reading?: Reading;
  onLearn: (name: string, outId: number) => void;
  onReset: (name: string) => void;
  onUpdate: (patch: Partial<Sensor>) => void;
  // Set by CalibrationSummary's "Re-teach flagged colours" — when present, the palette below
  // shows only the flagged colours (present one distinctly, Teach, repeat) instead of the full
  // list, each with a live distance-to-nearest-neighbour readout (see nearestTaughtColour)
  // computed against the *live* reading below, so repositioning the sample shows the number
  // move before committing Teach — re-teaching in the same physical spot otherwise just
  // recaptures the same too-close reading.
  focusWarnings?: ProximityWarning[];
  onClearFocus?: () => void;
}) {
  const focusNames = focusWarnings && new Set(focusWarnings.flatMap((w) => [w.a, w.b]));
  // Jump the view to the palette itself when a Re-teach focus is set — selecting the sensor tab
  // only gets you to the top of its card; the palette is further down past MONITOR/recipe/etc.
  const paletteRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (focusNames) paletteRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusNames]);

  const learned = new Map((sensor.colours ?? []).map((c) => [c.name, c]));
  // col_lego/as_lego send only the classified id (no RGB/spectrum) — the wheel below has no
  // live position to plot in that mode (see liveClassified()), so it's only worth showing if
  // there's at least one taught colour to browse.
  const isLegoIdOnly = sensor.transform === "col_lego" || sensor.transform === "as_lego";
  const hasTaughtColours = (sensor.colours ?? []).some((c) => c.learned);
  const stdNames = new Set(SPIKE_COLOURS.map((c) => c.name));
  const customs = (sensor.colours ?? []).filter((c) => !stdNames.has(c.name));
  const [editing, setEditing] = useState<string | null>(null);   // colour name whose ref is open
  const [hover, setHover] = useState<{
    name: string; outId: number; taught: boolean; swatch: string; h: number; s: number; r: number; g: number; b: number;
  } | null>(null);   // wheel dot currently hovered — drives the detail panel below the wheel

  const isAs = sensor.type === "as7341";
  const calibNames = isAs
    ? ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "Clear", "NIR"]
    : ["clear", "red", "green", "blue"];
  const refNames = isAs ? calibNames : ["r", "g", "b"];
  const refCount = isAs ? 10 : 3;

  const setCalib = (idx: number, v: number) => {
    const c = [...(sensor.calib ?? [])];
    c[idx] = v;
    onUpdate({ calib: c });
  };

  const setRef = (name: string, idx: number, v: number) => {
    onUpdate({
      colours: (sensor.colours ?? []).map((c) =>
        c.name === name ? { ...c, ref: c.ref.map((rv, k) => (k === idx ? v : rv)) } : c,
      ),
    });
  };

  const hexToRgb = (hex: string): [number, number, number] => {
    const n = parseInt(hex.replace("#", ""), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  };

  // Convert RGB (0-255) to HSV; return hue (0-360), saturation (0-1), value (0-1).
  const rgbToHsv = (r: number, g: number, b: number): { h: number; s: number; v: number } => {
    r /= 255; g /= 255; b /= 255;
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
    let h = 0;
    if (d !== 0) {
      if (mx === r) h = ((g - b) / d + (g < b ? 6 : 0)) * 60;
      else if (mx === g) h = ((b - r) / d + 2) * 60;
      else h = ((r - g) / d + 4) * 60;
    }
    return { h: h % 360, s: mx === 0 ? 0 : d / mx, v: mx };
  };

  // Wheel position for a colour. Chromatic colours (s above the threshold) go on the hue ring
  // at radius scaled by saturation, as normal. Achromatic colours (black/white/silver/grey —
  // s ~ 0) have *undefined* hue, which our rgbToHsv defaults to 0°; plotting them by hue would
  // stack every achromatic colour on top of each other (and on red, at hue 0°). Instead they're
  // spread along a fixed row inside the centre "grey" zone, ordered by brightness (v) so
  // black/silver/white land at distinct, readable positions.
  const ACHROMATIC_S = 0.12;
  const wheelPos = (h: number, s: number, v: number, minR: number, maxR: number, achromaticY: number) => {
    if (s < ACHROMATIC_S) {
      return { x: 100 + (v - 0.5) * 30, y: achromaticY };
    }
    const rad = (h * Math.PI) / 180;
    const radius = minR + s * (maxR - minR);
    return { x: 100 + radius * Math.cos(rad), y: 100 + radius * Math.sin(rad) };
  };

  // Live HSV from the streamed reading, for the current-colour marker on the wheel — for
  // transforms that carry an RGB/HSV triple (col_rgb255/col_hue/col_full/as_full), and the
  // AS7341's raw 10-channel spectrum and the TCS34725's raw 4-channel counts (both approximated
  // to RGB the same way the firmware/taught-dot swatches do). col_lego/as_lego (classified id
  // only, no RGB/spectrum in the reading at all) and as_dist (8 match scores, no single hue to
  // plot) return null here — see liveClassified() below for what those two show instead.
  const liveHsv = (): { h: number; s: number; v: number; fill: string } | null => {
    const v = reading?.values;
    if (!v) return null;
    const t = sensor.transform || "";
    if (t === "col_rgb255") {
      const { h, s, v: val } = rgbToHsv(v[0] ?? 0, v[1] ?? 0, v[2] ?? 0);
      return { h, s, v: val, fill: `rgb(${Math.round(v[0] ?? 0)},${Math.round(v[1] ?? 0)},${Math.round(v[2] ?? 0)})` };
    }
    if (t === "col_hue") {
      const h = v[0] ?? 0, s = (v[1] ?? 0) / 100, val = (v[2] ?? 0) / 100;
      return { h, s, v: val, fill: `hsl(${h}, ${Math.round(s * 100)}%, ${Math.round(val * 50)}%)` };
    }
    if (t === "col_full" || t === "as_full") {
      const r = ((v[2] ?? 0) / 1024) * 255, g = ((v[3] ?? 0) / 1024) * 255, b = ((v[4] ?? 0) / 1024) * 255;
      const { h, s, v: val } = rgbToHsv(r, g, b);
      return { h, s, v: val, fill: `rgb(${Math.round(r)},${Math.round(g)},${Math.round(b)})` };
    }
    if (isAs && t === "raw" && v.length >= 8) {
      let r = (v[6] ?? 0) + (v[7] ?? 0);          // F7+F8
      let g = (v[3] ?? 0) + (v[4] ?? 0);          // F4+F5
      let b = (v[1] ?? 0) + (v[2] ?? 0);          // F2+F3
      const mx = Math.max(r, g, b, 1);
      r = (r / mx) * 255; g = (g / mx) * 255; b = (b / mx) * 255;
      const { h, s, v: val } = rgbToHsv(r, g, b);
      return { h, s, v: val, fill: `rgb(${Math.round(r)},${Math.round(g)},${Math.round(b)})` };
    }
    if (!isAs && t === "raw" && v.length >= 4) {
      // TCS34725 raw: [clear, red, green, blue] 16-bit counts — same normalise-to-255 approach
      // as the AS7341 branch above, just from 3 channels directly instead of 8 filter pairs.
      // This one was simply missing before: the AS7341 raw case was handled, the TCS one wasn't,
      // so the wheel's live marker silently never appeared for a TCS sensor left in its default
      // "raw counts" convert mode.
      let r = v[1] ?? 0, g = v[2] ?? 0, b = v[3] ?? 0;
      const mx = Math.max(r, g, b, 1);
      r = (r / mx) * 255; g = (g / mx) * 255; b = (b / mx) * 255;
      const { h, s, v: val } = rgbToHsv(r, g, b);
      return { h, s, v: val, fill: `rgb(${Math.round(r)},${Math.round(g)},${Math.round(b)})` };
    }
    return null;
  };

  // The classified id/name, read straight off the reading — for col_lego/as_lego (id-only, no
  // RGB/spectrum to plot a wheel position from at all) this is the *only* live info available;
  // for col_full/as_full (which also carry the id, at the same index 0, alongside RGB) it's a
  // plain-text companion to the wheel's live position marker below, which only plots the raw
  // hue and previously left the classified name/id itself only visible by hovering a dot.
  const liveClassified = (): { text: string; swatch: string } | null => {
    const t = sensor.transform || "";
    if (t !== "col_lego" && t !== "as_lego" && t !== "col_full" && t !== "as_full") return null;
    const outId = reading?.values?.[0];
    if (outId === undefined) return null;
    if (outId < 0) return { text: "none", swatch: "#555" };
    const taught = (sensor.colours ?? []).find((c) => c.learned && c.out_id === outId);
    const nominal = SPIKE_COLOURS.find((c) => c.id === Math.round(outId));
    const name = taught?.name ?? nominal?.name ?? `id ${Math.round(outId)}`;
    const swatch = (taught && swatchFromRef(sensor.type, taught.ref, as7341SwatchGain(sensor, taught.ref))) || nominal?.swatch || "#888";
    return { text: `${name} (id ${Math.round(outId)})`, swatch };
  };

  // Seed a manual reference for a colour that has no learned/taught entry yet — starting point
  // is its nominal swatch colour (converted to the sensor's ref space), so "Edit" on a default
  // row lets you hand-tune without presenting a physical sample. Existing taught entries are
  // untouched; this only fires the first time you open Edit on a still-default colour.
  const seedManualRef = (name: string, outId: number, swatch: string | undefined) => {
    if (learned.get(name)?.learned) return;
    const [r, g, b] = hexToRgb(swatch ?? "#888888");
    const ref = new Array(refCount).fill(0);
    if (isAs) {
      // Rough inverse of the RGB approximation used for display: spread each channel across
      // its filter pair. Just a starting point — the sliders below refine it.
      ref[6] = ref[7] = r / 2; ref[3] = ref[4] = g / 2; ref[1] = ref[2] = b / 2;
      ref[8] = Math.max(r, g, b);   // Clear
    } else {
      ref[0] = r; ref[1] = g; ref[2] = b;
    }
    onUpdate({
      colours: [
        ...(sensor.colours ?? []).filter((c) => c.name !== name),
        { name, out_id: outId, learned: true, ref },
      ],
    });
  };

  const row = (name: string, outId: number, swatch: string | undefined, isCustom: boolean) => {
    const entry = learned.get(name);
    const taught = !!entry?.learned;
    const taughtSwatch = taught ? swatchFromRef(sensor.type, entry!.ref, as7341SwatchGain(sensor, entry!.ref)) : null;
    const open = editing === name;
    return (
      <div key={name} className={`colour-entry${open ? " open" : ""}`}>
        <div className="colour-row">
          <div className="colour-swatches">
            {swatch !== undefined ? (
              <span className="colour-swatch" title="nominal colour" style={{ background: swatch }} />
            ) : (
              <span className="colour-swatch placeholder" />
            )}
            {taughtSwatch ? (
              <span className="colour-swatch" title="taught colour (from the calibrated reference)" style={{ background: taughtSwatch }} />
            ) : (
              <span className="colour-swatch placeholder" />
            )}
          </div>
          <span>{name}</span>
          <span
            className="muted sm"
            title={[2, 5, 8, 11].includes(outId)
              ? "Not hub-native: official LEGO firmware's color() only recognises ids 0,1,3,4,6,7,9,10 and shows this id as a neighbouring colour — Pybricks and RGBI-word fields read it exactly"
              : undefined}
          >
            id {outId}{[2, 5, 8, 11].includes(outId) && <AlertTriangle size={11} strokeWidth={2.25} className="inline-icon warn-icon" style={{ marginLeft: 3 }} />}
          </span>
          <span className="muted sm">{taught ? "● taught" : "○ default"}</span>
          <button className="ghost sm" disabled={!!busy} title="Capture the current reading as this colour" onClick={() => onLearn(name, outId)}>
            Teach
          </button>
          <button
            className="ghost sm"
            title={taught ? "Manually fine-tune this colour's captured reference" : "Manually set this colour's reference (starts from its nominal colour) without presenting a sample"}
            onClick={() => {
              if (!taught) seedManualRef(name, outId, swatch);
              setEditing(open ? null : name);
            }}
          >
            {open ? "Close" : "Edit"}
          </button>
          {(taught || isCustom) ? (
            <button
              className="ghost sm danger"
              disabled={!!busy}
              title={isCustom ? "Erase this custom colour entirely, on the device" : "Clear this colour's taught reference, on the device — falls back to its default/nominal reference"}
              onClick={() => onReset(name)}
            >
              {isCustom ? "Delete" : "Reset"}
            </button>
          ) : <span />}
        </div>
        {focusWarnings && (() => {
          const fw = focusWarnings.find((w) => w.a === name || w.b === name);
          const live = fw && reading?.values && !reading.status?.startsWith("err")
            ? nearestTaughtColour(sensor, reading.values, fw.median, new Set([name]))
            : null;
          if (!live) return null;
          const fmtDist = (v: number) => v.toFixed(v < 1 ? 3 : 1);
          return (
            <p className="muted sm" style={{ margin: "0 0 6px", color: live.clear ? "var(--accent-2)" : "var(--warn)" }}>
              live: closest match is <b>{live.name}</b> at Δ{fmtDist(live.dist)}
              {live.clear
                ? " — clear of that pair's threshold, safe to Teach now"
                : ` — still below ${fmtDist(live.threshold)}; reposition (distance/angle/LED brightness) and watch this rise before Teaching`}
            </p>
          );
        })()}
        {actionError && actionError.colour === name && actionError.action !== "calibrate" && (
          <p className="warn" style={{ margin: "2px 0 6px", padding: "6px 8px", fontSize: "12px" }}>
            <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> {actionError.action === "teach" ? "Teach" : "Reset"} rejected — nothing was saved: {actionError.message}
          </p>
        )}
        {open && taught && (
          <div className="colour-ref-editor">
            {Array.from({ length: refCount }, (_, k) => {
              const v = entry!.ref[k] ?? 0;
              // Slider range must fit the ref's actual scale or the handle pins at full and
              // the value becomes uneditable: TCS refs are 0-255 RGB, but AS7341 refs are
              // post-whitecal (F/NIR peak-normalised to 1000) and ref[8] holds the RAW Clear
              // count (up to the 40000 full scale). Grow with the value so nothing ever pins.
              const base = !isAs ? 255 : k === 8 ? 40000 : 1200;
              const max = Math.max(base, Math.ceil((v * 1.5) / 100) * 100);
              return (
                <div key={k} className="calib-row" title={`taught reference — ${refNames[k]} (live)`}>
                  <span className="label">{refNames[k]}</span>
                  <input
                    type="range"
                    min={0}
                    max={max}
                    step={1}
                    value={v}
                    onChange={(e) => setRef(name, k, Number(e.target.value))}
                  />
                  <span className="value">{Math.round(v)}</span>
                </div>
              );
            })}
            <span className="muted sm">drag to fine-tune — swatches + wheel update live, Save to persist</span>
          </div>
        )}
      </div>
    );
  };

  // In-app dialog for adding a custom colour — replaces two chained browser prompt()s, whose
  // cancel button returned null and (via Number(null) === 0) used to teach the cancelled
  // colour anyway as id 0/black. Teach only ever fires from this dialog's explicit button.
  const [customDraft, setCustomDraft] = useState<{ name: string; id: string } | null>(null);
  const customDraftId = customDraft ? Number(customDraft.id) : NaN;
  const customDraftValid = !!customDraft && customDraft.name.trim().length > 0 &&
    Number.isInteger(customDraftId) && customDraftId >= 0 && customDraftId <= 255;
  const submitCustom = () => {
    if (!customDraft || !customDraftValid) return;
    onLearn(customDraft.name.trim(), customDraftId);   // captures the current reading immediately
    setCustomDraft(null);
  };

  const showStd = focusNames ? SPIKE_COLOURS.filter((c) => focusNames.has(c.name)) : SPIKE_COLOURS;
  const showCustoms = focusNames ? customs.filter((c) => focusNames.has(c.name)) : customs;

  return (
    <div className="colour-editor" ref={paletteRef}>
      <section className="colour-section">
        <div className="colour-section-head">
          <h4 className="colour-section-title">Colour palette</h4>
          <span className="muted sm">present a colour, then Teach</span>
        </div>
        {focusNames && (
          <div className="warn" style={{ marginBottom: 10, display: "flex", alignItems: "center", justifyContent: "space-between", gap: 10 }}>
            <span>Showing just the {focusNames.size} colour{focusNames.size > 1 ? "s" : ""} flagged as too close — present each one distinctly (different distance/LED brightness), then Teach.</span>
            <button className="ghost sm" onClick={onClearFocus}>Show all colours</button>
          </div>
        )}
        <div className="colour-rows">
          {showStd.map((c) => row(c.name, c.id, c.swatch, false))}
        </div>
        {showCustoms.length > 0 && (
          <>
            <div className="colour-subtitle">Custom colours</div>
            <div className="colour-rows">
              {showCustoms.map((c) => row(c.name, c.out_id, undefined, true))}
            </div>
          </>
        )}
        {/* A failed teach of a brand-new custom colour has no row yet to attach its error to —
            surface it here so the rejection is still visible right where the click happened. */}
        {actionError && actionError.action === "teach" && actionError.colour !== undefined &&
          !SPIKE_COLOURS.some((c) => c.name === actionError.colour) &&
          !customs.some((c) => c.name === actionError.colour) && (
          <p className="warn" style={{ margin: "6px 0 0", padding: "6px 8px", fontSize: "12px" }}>
            <AlertTriangle size={12} strokeWidth={2.25} className="inline-icon warn-icon" /> Teach "{actionError.colour}" rejected — nothing was saved: {actionError.message}
          </p>
        )}
        <button
          className="ghost sm"
          disabled={!!busy}
          style={{ marginTop: 10 }}
          title="Teach a colour outside the standard palette, captured from the current reading"
          onClick={() => setCustomDraft({ name: "", id: "12" })}
        >
          + add custom colour
        </button>
        <p className="muted sm" style={{ margin: "8px 0 0" }}>
          ℹ️ <b>none (id -1)</b> is automatic and never taught: the sensor reports it whenever
          nothing reflective is in range (e.g. pointed at mid-air) or no palette entry matches
          the reading — the hub's <code>color()</code> shows no colour for it.
        </p>
      </section>

      {customDraft && (
        <Modal
          title="Add custom colour"
          onClose={() => setCustomDraft(null)}
          actions={
            <>
              <button className="ghost sm" onClick={() => setCustomDraft(null)}>Cancel</button>
              <button className="ghost sm" disabled={!customDraftValid || !!busy} onClick={submitCustom}>
                Teach from current reading
              </button>
            </>
          }
        >
          <label className="field">
            <span className="muted sm">Colour name</span>
            <input
              type="text"
              autoFocus
              placeholder="e.g. my-teal"
              value={customDraft.name}
              onChange={(e) => setCustomDraft({ ...customDraft, name: e.target.value })}
              onKeyDown={(e) => { if (e.key === "Enter") submitCustom(); }}
            />
          </label>
          <label className="field">
            <span className="muted sm">Report id sent to the hub</span>
            <input
              type="number"
              min={0}
              max={255}
              value={customDraft.id}
              onChange={(e) => setCustomDraft({ ...customDraft, id: e.target.value })}
              onKeyDown={(e) => { if (e.key === "Enter") submitCustom(); }}
            />
          </label>
          <p className="muted sm" style={{ margin: 0 }}>
            0/1/3/4/6/7/9/10 show a named SPIKE colour on the hub; 2/5/8/11 are extra LPF2 ids
            (official hub firmware shows a neighbouring colour — Pybricks reads them exactly);
            &gt;11 = custom, hub shows none. Present the colour to the sensor before confirming —
            teaching captures the current reading.
          </p>
        </Modal>
      )}

      {/* Hue/saturation wheel: every standard colour (hollow, at its nominal hue) plus every
          taught/custom colour (solid, at its actual captured hue) — so overlaps against
          defaults are visible, not just overlaps between taught colours. Radius encodes
          saturation, not just a fixed ring: white/black/grey have near-zero saturation (hue is
          undefined for them — our hue calc arbitrarily returns 0°, i.e. "red"), so plotting
          purely by hue would stack them on top of red. Pulling low-saturation dots toward the
          centre keeps them visually distinct from genuinely red/saturated colours. */}
      <section className="colour-section">
        <div className="colour-section-head">
          <h4 className="colour-section-title">Colour wheel</h4>
          <span className="muted sm">hollow = default, solid = taught, pulsing ring = live — centre = black↔white by brightness</span>
        </div>
        {(() => {
          // col_lego/as_lego send only the classified id (no RGB/spectrum) — the wheel has
          // nothing to plot a live *position* from, so this shows the plain live value that mode
          // does give you instead of a live marker guessing at a wheel position.
          const live = liveClassified();
          if (!live) return null;
          return (
            <p className="muted sm" style={{ display: "flex", alignItems: "center", gap: 6, margin: "0 0 8px" }}>
              live:
              <span style={{ width: 12, height: 12, borderRadius: 3, background: live.swatch, border: "1px solid #fff", display: "inline-block" }} />
              <b>{live.text}</b>
            </p>
          );
        })()}
        {isLegoIdOnly && !hasTaughtColours ? (
          // Nothing worth plotting yet: no RGB/spectrum for a live position (see liveClassified
          // above) AND no taught dots to browse either — the wheel would just be the empty
          // default-only palette, which the live readout above already covers more plainly.
          <p className="muted sm">
            Colour wheel hidden in this mode — no live position to show (only the classified id
            is sent), and no colours taught yet to browse. Teach a colour below to see it here.
          </p>
        ) : (
        <div className="colour-wheel-wrap">
          <svg
            width={420} height={420} viewBox="0 0 200 200"
            style={{ border: "1px solid #555", borderRadius: "50%", boxShadow: "0 2px 12px rgba(0,0,0,0.4)" }}
          >
            <defs>
              {/* Left-to-right black→white gradient for the centre "achromatic" zone, matching
                  wheelPos's own dark-to-light placement (x = 100 + (v-0.5)*30) — so the fill
                  itself shows what the zone means (brightness, no hue) instead of a flat swatch
                  plus a "grey" label that didn't actually explain anything. */}
              <linearGradient id="achromaticGrad" x1="0%" y1="0%" x2="100%" y2="0%">
                <stop offset="0%" stopColor="#000" />
                <stop offset="100%" stopColor="#fff" />
              </linearGradient>
            </defs>
            {/* Hue ring (background) */}
            {Array.from({ length: 360 }, (_, deg) => {
              const rad = (deg * Math.PI) / 180;
              const x1 = 100 + 80 * Math.cos(rad);
              const y1 = 100 + 80 * Math.sin(rad);
              const x2 = 100 + 90 * Math.cos(rad);
              const y2 = 100 + 90 * Math.sin(rad);
              const hsl = `hsl(${deg}, 100%, 50%)`;
              return (
                <line key={deg} x1={x1} y1={y1} x2={x2} y2={y2} stroke={hsl} strokeWidth={1} />
              );
            })}
            {/* Faint saturation guide rings (25/50/75%) to help judge dot spread by eye. */}
            {[0.25, 0.5, 0.75].map((frac) => (
              <circle key={frac} cx={100} cy={100} r={24 + frac * 41} fill="none" stroke="#fff" strokeOpacity={0.08} strokeWidth={1} />
            ))}
            {/* Centre circle — the "achromatic" zone: low-saturation colours (black/white/silver)
                cluster here regardless of their (meaningless) hue, positioned left-to-right by
                brightness — the gradient fill shows that directly instead of a text label. */}
            <circle cx={100} cy={100} r={20} fill="url(#achromaticGrad)" stroke="#888" strokeWidth={0.5} />

            {/* Standard SPIKE colours — nominal hue/sat, hollow dot, unless taught (skip here; the
                taught dot below covers it at a slightly larger radius). */}
            {SPIKE_COLOURS.filter((c) => !learned.get(c.name)?.learned).map((c) => {
              const [r, g, b] = hexToRgb(c.swatch);
              const { h, s, v } = rgbToHsv(r, g, b);
              const { x, y } = wheelPos(h, s, v, 24, 65, 86);   // 24..65 radius when chromatic
              const info = { name: c.name, outId: c.id, taught: false, swatch: c.swatch, h, s, r, g, b };
              return (
                <g
                  key={`d-${c.name}`}
                  style={{ cursor: "pointer" }}
                  onMouseEnter={() => setHover(info)}
                  onMouseLeave={() => setHover((cur) => (cur?.name === c.name && !cur.taught ? null : cur))}
                >
                  <circle cx={x} cy={y} r={5} fill={c.swatch} fillOpacity={0.35} stroke={c.swatch} strokeWidth={1.5} />
                </g>
              );
            })}

            {/* Taught + custom colours — actual captured hue/sat, solid dot. */}
            {(sensor.colours ?? []).map((c) => {
              if (!c.learned || c.ref.length < 3) return null;
              const isAsType = sensor.type === "as7341";
              let r = c.ref[0], g = c.ref[1], b = c.ref[2];
              if (isAsType) {
                // Approximate RGB from spectrum
                r = (c.ref[6] ?? 0) + (c.ref[7] ?? 0);
                g = (c.ref[3] ?? 0) + (c.ref[4] ?? 0);
                b = (c.ref[1] ?? 0) + (c.ref[2] ?? 0);
                const mx = Math.max(r, g, b, 1);
                r = (r / mx) * 255;
                g = (g / mx) * 255;
                b = (b / mx) * 255;
              }
              const { h, s, v } = rgbToHsv(r, g, b);
              const { x, y } = wheelPos(h, s, v, 28, 75, 96);   // 28..75 radius when chromatic
              const dotSwatch = swatchFromRef(sensor.type, c.ref, as7341SwatchGain(sensor, c.ref)) ?? "#888";
              const info = { name: c.name, outId: c.out_id, taught: true, swatch: dotSwatch, h, s, r, g, b };
              return (
                <g
                  key={c.name}
                  style={{ cursor: "pointer" }}
                  onMouseEnter={() => setHover(info)}
                  onMouseLeave={() => setHover((cur) => (cur?.name === c.name && cur.taught ? null : cur))}
                >
                  <circle cx={x} cy={y} r={6} fill={dotSwatch} stroke="#fff" strokeWidth={1} />
                  <circle cx={x} cy={y} r={6} fill="none" stroke="#555" strokeWidth={0.5} />
                </g>
              );
            })}

            {/* Live marker — the sensor's *current* reading, updating every poll while streaming.
                A crosshair ring (not a filled dot) so it stays visible over/near existing dots. */}
            {(() => {
              const live = liveHsv();
              if (!live) return null;
              const { x, y } = wheelPos(live.h, live.s, live.v, 28, 75, 101);
              return (
                <g key="live" style={{ pointerEvents: "none" }}>
                  <circle cx={x} cy={y} r={9} fill="none" stroke={live.fill} strokeWidth={2}>
                    <animate attributeName="r" values="7;10;7" dur="1.4s" repeatCount="indefinite" />
                    <animate attributeName="opacity" values="1;0.4;1" dur="1.4s" repeatCount="indefinite" />
                  </circle>
                  <circle cx={x} cy={y} r={3} fill={live.fill} stroke="#fff" strokeWidth={1} />
                </g>
              );
            })()}
          </svg>

          {/* Detail panel — replaces native title tooltips (slow/inconsistent, no touch support)
              with a persistent readout of whichever dot is hovered. */}
          <div className="colour-hover-panel">
            {hover ? (
              <>
                <span style={{ width: 16, height: 16, borderRadius: 3, background: hover.swatch, border: "1px solid #fff", display: "inline-block", flexShrink: 0 }} />
                <span style={{ minWidth: 90 }}>
                  <b>{hover.name}</b> <span className="muted sm">({hover.taught ? "taught" : "default"})</span>
                </span>
                <span className="muted sm">id {hover.outId}</span>
                <span className="muted sm">h {hover.h.toFixed(0)}° s {(hover.s * 100).toFixed(0)}%</span>
                <span className="muted sm">rgb {Math.round(hover.r)},{Math.round(hover.g)},{Math.round(hover.b)}</span>
              </>
            ) : (
              <span className="muted sm">hover a dot for details</span>
            )}
          </div>
        </div>
        )}
      </section>

      {/* Live spectrum — the AS7341's native view. The hue/sat wheel only shows an RGB
          *approximation* of the 10-band reading; this shows the actual measured bands, which
          is what the spectral classifier actually compares. Needs the `raw` convert mode
          (10 channels) and live polling running. */}
      {isAs && (
        <section className="colour-section">
          <div className="colour-section-head">
            <h4 className="colour-section-title">Live spectrum</h4>
            <span className="muted sm">10 bands, scaled to the strongest</span>
          </div>
          {sensor.transform === "raw" && (reading?.values.length ?? 0) >= 10 ? (
            <SpectrumBars values={reading!.values} />
          ) : (
            <p className="muted sm" style={{ margin: 0 }}>
              Set convert mode to <b>raw</b> and start polling to see the live 10-band spectrum
              (other modes stream derived values, not the spectral channels).
            </p>
          )}
        </section>
      )}

      {/* White-balance calibration — the values Calibrate captured, as direct sliders. Lowering
          a channel brightens that channel's output (col_full/as_full divide raw by this ref),
          so this *is* the fine-tune — no separate gain step. */}
      {(sensor.calib?.length ?? 0) > 0 && (
        <section className="colour-section">
          <div className="colour-section-head">
            <h4 className="colour-section-title">White balance</h4>
            <span className="muted sm">calibrated — drag to fine-tune</span>
          </div>
          <div className="calib-rows">
            {(sensor.calib ?? []).map((v, k) => {
              const max = Math.max(1000, Math.ceil((v * 1.5) / 100) * 100);
              return (
                <div key={k} className="calib-row" title="white reference captured by Calibrate (live)">
                  <span className="label">{calibNames[k] ?? `c${k}`}</span>
                  <input
                    type="range"
                    min={1}
                    max={max}
                    step={1}
                    value={v}
                    onChange={(e) => setCalib(k, Number(e.target.value))}
                  />
                  <span className="value">{Math.round(v)}</span>
                </div>
              );
            })}
          </div>
          <p className="muted sm" style={{ margin: "8px 0 0" }}>
            Lower = brighter output for that channel. RGB, reflect and classification all derive
            from these — Save to persist, re-Teach colours if you change them a lot.
          </p>
        </section>
      )}

      {/* Reading filters — reduce jitter/misreads: smoothing dampens noisy raw channels before
          classification, debounce holds the last stable colour until a new one repeats. */}
      <section className="colour-section">
        <div className="colour-section-head">
          <h4 className="colour-section-title">Reading filters</h4>
          <span className="muted sm">reduce jumpy/misclassified colours</span>
        </div>
        <div className="calib-rows">
          <div className="calib-row" title="Exponential smoothing of the raw channels before classification — higher = steadier but slower to react">
            <span className="label">smoothing</span>
            <input
              type="range"
              min={0}
              max={0.95}
              step={0.05}
              value={sensor.colour_smooth ?? 0}
              onChange={(e) => onUpdate({ colour_smooth: Number(e.target.value) })}
            />
            <span className="value">
              {(sensor.colour_smooth ?? 0) === 0 ? "off" : `${Math.round((sensor.colour_smooth ?? 0) * 100)}%`}
            </span>
          </div>
          <div className="calib-row" title="Require this many consecutive matching classifications before the reported colour changes — stops single-sample flicker">
            <span className="label">debounce</span>
            <input
              type="range"
              min={0}
              max={10}
              step={1}
              value={sensor.colour_debounce ?? 0}
              onChange={(e) => onUpdate({ colour_debounce: Number(e.target.value) })}
            />
            <span className="value">
              {(sensor.colour_debounce ?? 0) === 0 ? "off" : `${sensor.colour_debounce} reads`}
            </span>
          </div>
        </div>
        <p className="muted sm" style={{ margin: "8px 0 0" }}>
          Smoothing steadies noisy raw readings; debounce stops the reported colour flickering
          between two ids. Both default off (unchanged behaviour) — Save to apply.
        </p>
      </section>
    </div>
  );
}
