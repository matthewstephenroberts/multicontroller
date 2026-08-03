import type { ReactNode } from "react";
import type { DisplayConfig, DisplayController, DisplayMode } from "../types";
import { HelpTip } from "./HelpTip";

interface Props {
  display: DisplayConfig;
  busy: string | null;
  // Board capability snapshot from get_config, available at connect time (no scan needed) —
  // lets "Reset to board defaults" recover from a stale/wrong saved display config even if the
  // user never visits the Scan tab.
  board: { has_display: boolean; tft_controller?: string };
  // Kid/easy view (false) hides pin/geometry fields (cs/dc/rst/bl, width/height, gaps, mirror,
  // invert) — a kid picking a controller preset and a mode doesn't need to touch wiring/geometry.
  advanced: boolean;
  onChange: (next: DisplayConfig) => void;
  onSave: () => void;
  onResetToBoardDefaults: () => void;
}

// Sensible geometry presets per controller — also used by App.tsx's "Use as display" (scan
// result -> display config) so both places agree on what each controller needs.
export const DISPLAY_PRESETS: Record<DisplayController, Partial<DisplayConfig>> = {
  st7789: { bus: "spi", width: 240, height: 135, x_gap: 40, y_gap: 53, invert: true },
  ili9341: { bus: "spi", width: 320, height: 240, x_gap: 0, y_gap: 0, invert: false },
  // M5Stack AtomS3R's onboard 0.85" panel — 128x128 visible of a 128x160 controller memory,
  // so the extra 32 rows need a y_gap (see board_config.h's BOARD_ATOMS3R section).
  gc9107: { bus: "spi", width: 128, height: 128, x_gap: 0, y_gap: 32, invert: false },
  // The AtomS3R's onboard panel is one of two different ICs depending on hardware revision —
  // if gc9107 reports success in the boot log but nothing renders, try this one instead. 132x132
  // controller memory, 128x128 visible. x_gap=1/y_gap=2 is M5GFX's own offset_x=2/offset_y=1
  // transposed through the firmware's swap_xy=true (see board_config.h's BOARD_ATOMS3R section);
  // mirror_y stands in for M5GFX's offset_rotation=2. Verified pixel-exact on real hardware.
  st7735s: { bus: "spi", width: 128, height: 128, x_gap: 1, y_gap: 2, mirror_x: false, mirror_y: true, invert: true },
  ssd1306: { bus: "i2c", width: 128, height: 64, x_gap: 0, y_gap: 0, invert: false, addr: 0x3c },
};

export function DisplayConfigForm(p: Props) {
  const d = p.display;
  const set = (patch: Partial<DisplayConfig>) => p.onChange({ ...d, ...patch });

  const setController = (controller: DisplayController) =>
    set({ controller, ...DISPLAY_PRESETS[controller] });

  const isSpi = d.bus === "spi";

  return (
    <section className="card">
      <div className="card-head">
        <h2>
          Display
          <HelpTip>
            If your board has its own little screen built in, turn it on here and choose what it
            shows — like a name tag, or the numbers from your favourite sensor.
          </HelpTip>
        </h2>
        <div className="row gap">
          {p.board.has_display && (
            <button
              className="ghost sm"
              onClick={p.onResetToBoardDefaults}
              title={`Reset controller and cs/dc/rst/bl pins to what this board's firmware reports as actually wired (${p.board.tft_controller ?? "?"}) — recovers from a stale or wrong saved display config without needing to run a scan first`}
            >
              Reset to board defaults
            </button>
          )}
          <label className="check">
            <input
              type="checkbox"
              checked={d.enabled}
              onChange={(e) => set({ enabled: e.target.checked })}
            />
            enabled
          </label>
        </div>
      </div>

      <p className="muted sm">
        Enable a screen and choose which sensors appear on it (per-sensor below). Every mode
        headers with the board's own name (rename it from the header pencil icon, top right) —
        pick <b>summary</b>/<b>paged</b> to read exact values, or <b>tiles</b> for an at-a-glance
        visual grid. Changing the controller/pins or enabling a previously-disabled display
        takes effect after a device reboot; mode and per-sensor show/page apply live. If the
        controller/pins look wrong (e.g. after a firmware update changed the board's onboard
        panel), use <b>Reset to board defaults</b> above rather than editing them by hand.
      </p>

      <div className="fields">
        <Field label="controller">
          <select value={d.controller} onChange={(e) => setController(e.target.value as DisplayController)}>
            <option value="st7789">st7789 (SPI)</option>
            <option value="ili9341">ili9341 (SPI)</option>
            <option value="gc9107">gc9107 (SPI)</option>
            <option value="st7735s">st7735s (SPI)</option>
            <option value="ssd1306">ssd1306 (I2C)</option>
          </select>
        </Field>

        <Field label="mode">
          <select
            value={d.mode}
            onChange={(e) => set({ mode: e.target.value as DisplayMode })}
            title={
              d.mode === "tiles"
                ? "Visual grid — one tile per shown sensor: colour sensors show their swatch, distance/percentage-like values show a fill bar, quantized gamepad sticks show a centred bipolar bar, boolean-shaped values (detected/state) show filled-vs-outline. Groups by page like paged mode."
                : d.mode === "paged"
                  ? "Text, one screen per sensor page group — BOOT button cycles pages"
                  : "Text, every shown sensor listed on one scrolling screen"
            }
          >
            <option value="summary">summary (text)</option>
            <option value="paged">paged (text, BOOT btn)</option>
            <option value="tiles">tiles (visual grid)</option>
          </select>
        </Field>

        <Num
          label="auto-sleep after (s)"
          value={d.sleep_after_s ?? -1}
          onChange={(v) => set({ sleep_after_s: v < -1 ? -1 : v })}
          title="Turns the backlight off after this many seconds with no BOOT-button press — saves power/panel life on a display nobody's actively looking at most of the time. Sensors/config keep updating regardless; a press instantly wakes it. -1 = always on (default)."
        />

        <Field label={`brightness ${d.brightness ?? 100}%`}>
          <input
            type="range"
            min={0}
            max={100}
            step={5}
            value={d.brightness ?? 100}
            title="Backlight brightness — applies on Save, no reboot needed. Real dimming on panels with a PWM-capable backlight driver (e.g. AtomS3R); boards with a plain on/off backlight GPIO treat anything above 0 as fully on."
            onChange={(e) => set({ brightness: Number(e.target.value) })}
          />
        </Field>

        <label
          className="check"
          title="Shows the board's name for a moment before the first status/tile screen. Turn off to skip straight to it for the fastest possible startup — e.g. if this display gets power-cycled often."
        >
          <input
            type="checkbox"
            checked={d.show_boot_logo ?? true}
            onChange={(e) => set({ show_boot_logo: e.target.checked })}
          />
          boot logo
        </label>

        {d.mode === "tiles" && (
          <Field label="tiles per screen">
            <select
              value={d.tiles_per_page ?? 0}
              onChange={(e) => set({ tiles_per_page: Number(e.target.value) as 0 | 1 | 2 | 4 | 8 })}
              title="auto fits as many tiles as reasonably legible by the panel's own aspect ratio — more shown sensors means smaller tiles. A fixed count instead keeps a clean 1x1/2x1/2x2/4x2 layout and spills extra sensors onto further BOOT-cycled screens, trading legibility for count."
            >
              <option value={0}>auto (fit all)</option>
              <option value={1}>1</option>
              <option value={2}>2</option>
              <option value={4}>4</option>
              <option value={8}>8</option>
            </select>
          </Field>
        )}

        {d.mode === "tiles" && (
          <label
            className="check"
            title="Group tiles into blocks by sensor category (Distance, Colour, Motion, Environment, Line/IR, Input, Other) — the same categories the Dashboard's own 'group by type' toggle uses — instead of one flat grid of every shown value. Ignored alongside a fixed tiles-per-screen count above."
          >
            <input
              type="checkbox"
              checked={d.group_tiles ?? false}
              onChange={(e) => set({ group_tiles: e.target.checked })}
            />
            group tiles by sensor type
          </label>
        )}

        {p.advanced && (isSpi ? (
          <>
            <Num label="cs" value={d.cs} onChange={(v) => set({ cs: v })} />
            <Num label="dc" value={d.dc} onChange={(v) => set({ dc: v })} />
            <Num label="rst" value={d.rst} onChange={(v) => set({ rst: v })} />
            <Num label="bl" value={d.bl} onChange={(v) => set({ bl: v })} />
          </>
        ) : (
          <Num label="addr" value={d.addr} onChange={(v) => set({ addr: v })} />
        ))}

        {p.advanced && (
          <>
            <Num label="width" value={d.width} onChange={(v) => set({ width: v })} />
            <Num label="height" value={d.height} onChange={(v) => set({ height: v })} />
            <Num label="x_gap" value={d.x_gap} onChange={(v) => set({ x_gap: v })} />
            <Num label="y_gap" value={d.y_gap} onChange={(v) => set({ y_gap: v })} />

            <label className="check">
              <input type="checkbox" checked={d.mirror_x} onChange={(e) => set({ mirror_x: e.target.checked })} />
              mirror_x
            </label>
            <label className="check">
              <input type="checkbox" checked={d.mirror_y} onChange={(e) => set({ mirror_y: e.target.checked })} />
              mirror_y
            </label>
            <label className="check">
              <input type="checkbox" checked={d.invert} onChange={(e) => set({ invert: e.target.checked })} />
              invert
            </label>
          </>
        )}
      </div>
    </section>
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

function Num({ label, value, onChange, title }: { label: string; value: number; onChange: (v: number) => void; title?: string }) {
  return (
    <Field label={label}>
      <input
        type="number"
        value={value}
        title={title}
        onFocus={(e) => e.currentTarget.select()}
        onChange={(e) => onChange(e.target.value === "" ? 0 : Number(e.target.value))}
      />
    </Field>
  );
}
