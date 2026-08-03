import type { Reading, Sensor } from "../types";
import { AlertTriangle } from "lucide-react";

interface Props {
  sensors: Sensor[];
  readings: Record<number, Reading>;
}

function Stick({ x, y, label, stickRange }: { x: number; y: number; label: string; stickRange: number }) {
  return (
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

const GAMEPAD_BUTTONS = [
  { name: "A", bit: 0 }, { name: "B", bit: 1 }, { name: "X", bit: 2 }, { name: "Y", bit: 3 },
  { name: "LB", bit: 4 }, { name: "RB", bit: 5 }, { name: "Back", bit: 6 }, { name: "Start", bit: 7 },
  { name: "L3", bit: 8 }, { name: "R3", bit: 9 },
];

// Live visualization of gamepad state (sticks, triggers, buttons)
export function GamepadVisualization(p: Props) {
  const pad = p.sensors.find((s) => s.type === "gamepad" && s.enabled)
    ?? p.sensors.find((s) => s.type === "gamepad");
  const r = pad ? p.readings[pad.id] : undefined;
  const v = r?.values ?? [];
  const buttons = v[0] ?? 0;
  const [lx, ly, rx, ry] = [v[1] ?? 0, v[2] ?? 0, v[3] ?? 0, v[4] ?? 0];
  const [lt, rt] = [v[5] ?? 0, v[6] ?? 0];

  const digital = pad?.transform === "pad_digital";
  const stickRange = digital ? 7 : 32768;
  const trigMax = digital ? 15 : 1023;

  if (!pad) return null;

  if (!r) {
    return (
      <p className="muted sm">
        <AlertTriangle size={13} strokeWidth={2.25} className="inline-icon warn-icon" /> No data yet — make sure the gamepad sensor is <b>enabled</b> and click{" "}
        <b>Start polling</b> in the Dashboard to see live gamepad state.
      </p>
    );
  }

  return (
    <div style={{ display: "flex", gap: 16, flexWrap: "wrap", alignItems: "flex-start" }}>
      {Stick({ x: lx, y: ly, label: "L stick", stickRange })}
      {Stick({ x: rx, y: ry, label: "R stick", stickRange })}
      <div>
        <div className="muted sm">triggers</div>
        <div className="row gap">
          {Bar({ v: lt, max: trigMax, label: "LT" })}
          {Bar({ v: rt, max: trigMax, label: "RT" })}
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
  );
}
