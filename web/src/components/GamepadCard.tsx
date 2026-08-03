import { useRef, useState } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import type { Reading, Sensor, VirtualGamepadState } from "../types";
import { GAMEPAD_BUTTONS } from "../types";
import { AlertTriangle } from "lucide-react";

interface HidStatus {
  connected: boolean;
  name: string;
}

interface Props {
  sensors: Sensor[];
  readings: Record<number, Reading>;
  hid: HidStatus;
  hidVirtual: boolean;
  streaming: boolean;
  busy: string | null;
  onScan: () => void;
  onForget: () => void;
  onToggleVirtual: (enabled: boolean) => void;
  onSetVirtualState: (state: VirtualGamepadState) => Promise<void>;
  onAddGamepadSensor: () => void;
}

const BLANK_STATE: VirtualGamepadState = { buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0, dpad: 0 };

// 8-way hat from 4 direction booleans (0 = released, 1 = up, clockwise to 8 = up-left) —
// matches the encoding hid_host.c decodes off a real pad, so the virtual dpad channel behaves
// the same as a physical one for anything reading it directly instead of the buttons bitmask.
function dpadHat(up: boolean, down: boolean, left: boolean, right: boolean): number {
  if (up && right) return 2;
  if (down && right) return 4;
  if (down && left) return 6;
  if (up && left) return 8;
  if (up) return 1;
  if (right) return 3;
  if (down) return 5;
  if (left) return 7;
  return 0;
}

// Live BLE-HID game controller (Xbox Series) — pair/forget + a live button/stick visualiser
// fed by the `gamepad` sensor's reading stream. Map its `buttons`/axis values to LEGO fields
// in the emitter card to send controller state to the hub. Also offers a *virtual* controller:
// an on-screen layout that drives the same `gamepad` sensor with no physical pad connected —
// press/drag here to test the LEGO-field mapping, a hub program, or the dashboard without
// digging out a controller.
export function GamepadCard(p: Props) {
  // Prefer an *enabled* gamepad sensor — the scheduler never polls a disabled one, so its
  // reading is permanently stale (all zeros/last value), which reads as "the sticks/triggers/
  // buttons don't move" with no indication why. Falls back to any gamepad sensor (even
  // disabled) so "+ Add gamepad sensor" still correctly hides once one exists.
  const pad = p.sensors.find((s) => s.type === "gamepad" && s.enabled)
    ?? p.sensors.find((s) => s.type === "gamepad");
  const r = pad ? p.readings[pad.id] : undefined;
  const v = r?.values ?? [];
  const buttons = v[0] ?? 0;
  const [lx, ly, rx, ry] = [v[1] ?? 0, v[2] ?? 0, v[3] ?? 0, v[4] ?? 0];
  const [lt, rt] = [v[5] ?? 0, v[6] ?? 0];
  // "raw" (full HID range: sticks ±32767, triggers 0-1023) and "digital" (pad_digital: sticks
  // ±7, triggers 0-15) put the *same* fields at the same value indices, just quantized to much
  // smaller numbers — this visualiser used to always divide by the raw-mode range, so in digital
  // mode a stick's ±7 barely nudged the dot a fraction of a pixel and a trigger's 0-15 never
  // filled more than ~1.5% of the bar: technically "moving", but imperceptibly so. Scale by
  // whichever range the sensor's actual transform produces.
  const digital = pad?.transform === "pad_digital";
  const stickRange = digital ? 7 : 32768;
  const trigMax = digital ? 15 : 1023;

  const stick = (x: number, y: number, label: string) => (
    <div style={{ textAlign: "center" }}>
      <div style={{ position: "relative", width: 56, height: 56, borderRadius: "50%", background: "rgba(127,127,127,0.15)", border: "1px solid #444" }}>
        <div
          style={{
            position: "absolute", width: 12, height: 12, borderRadius: "50%", background: "#4ea1e0",
            left: 22 + (x / stickRange) * 20, top: 22 + (y / stickRange) * 20,
          }}
        />
      </div>
      <span className="muted sm">{label}</span>
    </div>
  );

  return (
    <section className="card">
      <div className="card-head">
        <h2>Game controller (BLE-HID)</h2>
        <div className="row gap">
          {/* Connection status now lives in the app header, next to the board's own dot. */}
          <button
            className="primary sm"
            disabled={!!p.busy}
            title="Usually not needed — the board scans and reconnects on its own. Use this to retry immediately after putting a controller in pairing mode."
            onClick={p.onScan}
          >
            {p.busy === "hid_scan" ? "Scanning…" : "Pair / Scan"}
          </button>
          <button
            className="ghost sm danger"
            disabled={!!p.busy}
            title="Unpair: erase the stored bond so the board stops auto-reconnecting this controller"
            onClick={p.onForget}
          >
            Forget
          </button>
        </div>
      </div>

      <p className="muted sm">
        Connect an <b>Xbox Series</b> controller over Bluetooth LE — the board scans{" "}
        <b>automatically</b> (at boot and whenever the controller drops), so a previously paired
        pad reconnects on its own; for a brand-new pad just hold its pair button until it
        flashes (Pair / Scan only forces an immediate retry). It appears as a{" "}
        <code>gamepad</code> sensor — map its <code>buttons</code> / sticks to LEGO fields in the
        emitter card to drive a hub program. (ESP32-S3 is BLE-only: PS4/PS5 and other
        Classic-only pads won't connect.)
      </p>

      <label className="check" title="Drive the gamepad sensor from an on-screen controller instead of a real pad">
        <input
          type="checkbox"
          checked={p.hidVirtual}
          disabled={p.busy === "hid_virtual"}
          onChange={(e) => p.onToggleVirtual(e.target.checked)}
        />
        virtual controller (no pad needed)
      </label>

      {p.hidVirtual && (
        <VirtualPad onSetState={p.onSetVirtualState} />
      )}

      {!pad && (
        <p className="muted sm">
          No <code>gamepad</code> sensor yet — it has to exist in the sensor list (Configure card)
          before its buttons/sticks can be mapped to a LEGO field or shown on the dashboard, real
          pad or virtual.{" "}
          <button className="ghost sm" onClick={p.onAddGamepadSensor}>+ Add gamepad sensor</button>
        </p>
      )}

      {pad && !p.streaming && (
        <p className="muted sm">
          <AlertTriangle size={13} strokeWidth={2.25} className="inline-icon warn-icon" /> Polling is stopped — the live values below (and the virtual controller's effect on
          them) won't update until you click <b>Start polling</b> in the Dashboard card. If you
          just added the <code>gamepad</code> sensor, also click <b>Save to device</b> first, or
          the board doesn't know about it yet.
        </p>
      )}

      {pad && !pad.enabled && (
        <p className="muted sm">
          <AlertTriangle size={13} strokeWidth={2.25} className="inline-icon warn-icon" /> This <code>gamepad</code> sensor is disabled — the board never polls it, so the
          values below are frozen (not a connection problem). Enable it in the Sensors tab and
          Save to device.
        </p>
      )}

      {pad && (
        <div className="row gap" style={{ alignItems: "center", flexWrap: "wrap", gap: 16 }}>
          {stick(lx, ly, "L stick")}
          {stick(rx, ry, "R stick")}
          <div>
            <div className="muted sm">triggers</div>
            <div className="row gap">
              <Bar v={lt} max={trigMax} label="LT" />
              <Bar v={rt} max={trigMax} label="RT" />
            </div>
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "repeat(4, auto)", gap: 4 }}>
            {GAMEPAD_BUTTONS.map((b) => {
              const on = (buttons & (1 << b.bit)) !== 0;
              return (
                <span
                  key={b.bit}
                  style={{
                    padding: "2px 6px", borderRadius: 4, fontSize: "0.8em", textAlign: "center",
                    background: on ? "#4ea1e0" : "rgba(127,127,127,0.15)",
                    color: on ? "#000" : "inherit", border: "1px solid #444",
                  }}
                >
                  {b.name}
                </span>
              );
            })}
          </div>
        </div>
      )}
    </section>
  );
}

function Bar({ v, max, label }: { v: number; max: number; label: string }) {
  return (
    <div style={{ textAlign: "center" }}>
      <div style={{ width: 16, height: 48, background: "rgba(127,127,127,0.15)", borderRadius: 3, display: "flex", alignItems: "flex-end", border: "1px solid #444" }}>
        <div style={{ width: "100%", height: `${Math.min(100, (v / max) * 100)}%`, background: "#4ea1e0", borderRadius: 3 }} />
      </div>
      <span className="muted sm">{label}</span>
    </div>
  );
}

// Interactive on-screen Xbox layout. Holds its own local state (buttons/sticks/triggers/dpad)
// and pushes a full snapshot to the board on every change, paced to the BLE connection's actual
// round-trip (see trySend() below) rather than a fixed rate.
function VirtualPad({ onSetState }: { onSetState: (state: VirtualGamepadState) => Promise<void> }) {
  const [state, setState] = useState<VirtualGamepadState>(BLANK_STATE);
  const [dirs, setDirs] = useState({ up: false, down: false, left: false, right: false });
  const sending = useRef(false);
  const queued = useRef<VirtualGamepadState | null>(null);
  const lStickRef = useRef<HTMLDivElement>(null);
  const rStickRef = useRef<HTMLDivElement>(null);

  // Paced by the actual BLE round-trip, not a fixed timer: at most one hid_set_state request is
  // ever in flight. A stick drag or a mashed button fires push() far faster than a BLE write can
  // complete — queuing every one of those (or firing them concurrently, which the transport used
  // to allow) either wedges the GATT connection or piles up an ever-growing backlog that makes
  // the whole app feel locked up while it drains. Instead, only the latest state is kept while a
  // send is in flight; once it resolves, that latest (and only that) gets sent next.
  const trySend = () => {
    if (sending.current) return;
    const next = queued.current;
    if (!next) return;
    queued.current = null;
    sending.current = true;
    onSetState(next).finally(() => {
      sending.current = false;
      trySend();
    });
  };

  const push = (next: VirtualGamepadState) => {
    setState(next);
    queued.current = next;
    trySend();
  };

  const setButton = (bit: number, on: boolean) => {
    const buttons = on ? state.buttons | (1 << bit) : state.buttons & ~(1 << bit);
    push({ ...state, buttons });
  };

  const setDir = (dir: keyof typeof dirs, on: boolean) => {
    const next = { ...dirs, [dir]: on };
    setDirs(next);
    const dpad = dpadHat(next.up, next.down, next.left, next.right);
    const bit = { up: 12, down: 13, left: 14, right: 15 }[dir];
    const buttons = on ? state.buttons | (1 << bit) : state.buttons & ~(1 << bit);
    push({ ...state, buttons, dpad });
  };

  const stickPad = (axis: "l" | "r", label: string) => {
    const xKey = axis === "l" ? "lx" : "rx";
    const yKey = axis === "l" ? "ly" : "ry";
    const ref = axis === "l" ? lStickRef : rStickRef;

    const onMove = (e: ReactPointerEvent) => {
      const el = ref.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      const cx = rect.left + rect.width / 2, cy = rect.top + rect.height / 2;
      const rMax = rect.width / 2;
      let dx = (e.clientX - cx) / rMax, dy = (e.clientY - cy) / rMax;
      const mag = Math.hypot(dx, dy);
      if (mag > 1) { dx /= mag; dy /= mag; }
      push({ ...state, [xKey]: Math.round(dx * 32767), [yKey]: Math.round(dy * 32767) } as VirtualGamepadState);
    };

    const onUp = () => push({ ...state, [xKey]: 0, [yKey]: 0 } as VirtualGamepadState);

    const x = (state[xKey] as number) / 32768, y = (state[yKey] as number) / 32768;

    return (
      <div style={{ textAlign: "center" }}>
        <div
          ref={ref}
          onPointerDown={(e) => { e.currentTarget.setPointerCapture(e.pointerId); onMove(e); }}
          onPointerMove={(e) => { if (e.buttons) onMove(e); }}
          onPointerUp={onUp}
          onPointerLeave={(e) => { if (e.buttons) onUp(); }}
          style={{
            position: "relative", width: 72, height: 72, borderRadius: "50%",
            background: "rgba(127,127,127,0.15)", border: "1px solid #444", cursor: "grab", touchAction: "none",
          }}
        >
          <div
            style={{
              position: "absolute", width: 16, height: 16, borderRadius: "50%", background: "#4ea1e0",
              left: 28 + x * 28, top: 28 + y * 28, pointerEvents: "none",
            }}
          />
        </div>
        <span className="muted sm">{label}</span>
      </div>
    );
  };

  const trigger = (key: "lt" | "rt", label: string) => (
    <div style={{ textAlign: "center" }}>
      <input
        type="range"
        min={0}
        max={1023}
        value={state[key]}
        style={{ writingMode: "vertical-lr", direction: "rtl", height: 56, width: 20 }}
        onChange={(e) => push({ ...state, [key]: Number(e.target.value) } as VirtualGamepadState)}
      />
      <div>
        <span className="muted sm">{label}</span>
      </div>
    </div>
  );

  const dirBtn = (dir: keyof typeof dirs, label: string) => (
    <button
      className="ghost sm"
      style={{ background: dirs[dir] ? "#4ea1e0" : undefined, color: dirs[dir] ? "#000" : undefined }}
      onPointerDown={(e) => { e.preventDefault(); setDir(dir, true); }}
      onPointerUp={() => setDir(dir, false)}
      onPointerLeave={() => dirs[dir] && setDir(dir, false)}
    >
      {label}
    </button>
  );

  return (
    <div className="row gap" style={{ alignItems: "center", flexWrap: "wrap", gap: 16, margin: "6px 0" }}>
      {stickPad("l", "L stick")}
      {stickPad("r", "R stick")}
      <div className="row gap">
        {trigger("lt", "LT")}
        {trigger("rt", "RT")}
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 32px)", gridTemplateRows: "repeat(3, 32px)", gap: 2 }}>
        <span />{dirBtn("up", "↑")}<span />
        {dirBtn("left", "←")}<span />{dirBtn("right", "→")}
        <span />{dirBtn("down", "↓")}<span />
      </div>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(4, auto)", gap: 4 }}>
        {GAMEPAD_BUTTONS.filter((b) => b.bit < 12).map((b) => {
          const on = (state.buttons & (1 << b.bit)) !== 0;
          return (
            <button
              key={b.bit}
              className="ghost sm"
              style={{ background: on ? "#4ea1e0" : undefined, color: on ? "#000" : undefined }}
              onPointerDown={(e) => { e.preventDefault(); setButton(b.bit, true); }}
              onPointerUp={() => setButton(b.bit, false)}
              onPointerLeave={() => (state.buttons & (1 << b.bit)) !== 0 && setButton(b.bit, false)}
            >
              {b.name}
            </button>
          );
        })}
      </div>
      <span className="muted sm">press/drag to drive the gamepad sensor — release to zero sticks/triggers, buttons/dpad stay latched while held</span>
    </div>
  );
}
