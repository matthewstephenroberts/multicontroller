import { useEffect, useRef, useState } from "react";
import type { PointerEvent as ReactPointerEvent, ReactNode } from "react";
import type { VirtualGamepadState } from "../types";
import { X, ArrowUp, ArrowDown, ArrowLeft, ArrowRight } from "lucide-react";

interface Props {
  open: boolean;
  onClose: () => void;
  onToggleEnabled: (enabled: boolean) => void;
  onSetState: (state: VirtualGamepadState) => Promise<void>;
}

const BLANK_STATE: VirtualGamepadState = { buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0, dpad: 0 };

const GAMEPAD_BUTTONS = [
  { name: "A", bit: 0 }, { name: "B", bit: 1 }, { name: "X", bit: 2 }, { name: "Y", bit: 3 },
  { name: "LB", bit: 4 }, { name: "RB", bit: 5 }, { name: "Back", bit: 6 }, { name: "Start", bit: 7 },
  { name: "L3", bit: 8 }, { name: "R3", bit: 9 },
];

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

// Floating modal with virtual Xbox controller for testing without a real pad. Opening it is the
// only "enable" step a kid should need — no separate switch to remember — so the virtual
// controller is enabled on the device the moment this opens and disabled the moment it closes
// (including when the parent force-closes it, e.g. on disconnect).
export function GamepadVirtualModal(p: Props) {
  const [state, setState] = useState<VirtualGamepadState>(BLANK_STATE);
  const [dirs, setDirs] = useState({ up: false, down: false, left: false, right: false });
  const sending = useRef(false);
  const queued = useRef<VirtualGamepadState | null>(null);
  const lStickRef = useRef<HTMLDivElement>(null);
  const rStickRef = useRef<HTMLDivElement>(null);

  // Offset from the default bottom-right anchor, dragged via the header — lets the modal be
  // moved out of the way of whatever sensor card it's currently covering, without pinning it to
  // one fixed spot. Persists across open/close within the session (not reset on close) since a
  // kid who's found a spot that works shouldn't have to re-drag it every time they reopen it.
  const [pos, setPos] = useState({ x: 0, y: 0 });
  const dragState = useRef<{ startX: number; startY: number; posX: number; posY: number } | null>(null);
  const panelRef = useRef<HTMLDivElement>(null);

  const onHeaderPointerDown = (e: ReactPointerEvent) => {
    // Only the header itself starts a drag — the close button inside it needs its own click to
    // still work, not get swallowed as a tiny accidental drag.
    if ((e.target as HTMLElement).closest("button")) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    dragState.current = { startX: e.clientX, startY: e.clientY, posX: pos.x, posY: pos.y };
  };
  const onHeaderPointerMove = (e: ReactPointerEvent) => {
    const d = dragState.current;
    if (!d) return;
    const next = { x: d.posX + (e.clientX - d.startX), y: d.posY + (e.clientY - d.startY) };
    // The panel is anchored via `bottom: 16, right: 16` and this offset is applied as a
    // translate on top of that — so x/y more NEGATIVE moves it left/up (toward the top-left,
    // into view), more POSITIVE would push it further past the right/bottom edge (off-screen).
    // Clamp so at least a corner of the panel always stays reachable in every direction: never
    // push it further right/down than its default anchored spot (0), and never drag it further
    // left/up than the point where its trailing edge would leave the viewport entirely.
    const el = panelRef.current;
    const w = el?.offsetWidth ?? 420, h = el?.offsetHeight ?? 300;
    const minX = -(window.innerWidth - w - 16 - 16);   // left edge stops at the viewport's left edge
    const minY = -(window.innerHeight - h - 16 - 16);  // top edge stops at the viewport's top edge
    next.x = Math.max(minX, Math.min(0, next.x));
    next.y = Math.max(minY, Math.min(0, next.y));
    setPos(next);
  };
  const onHeaderPointerUp = () => { dragState.current = null; };

  useEffect(() => {
    p.onToggleEnabled(p.open);
    // Only re-run when open flips — onToggleEnabled/onSetState are inline closures from the
    // parent that change identity every render, and including them would fire this on every
    // keystroke elsewhere in the app instead of just open/close transitions.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [p.open]);

  // hid_set_state is a full command/response round-trip over the same BLE link (and mbuf pool)
  // that regular sensor-reading notifications use — "only one in flight" alone isn't enough of a
  // limit, since on a fast/local connection a round-trip can complete in well under 50ms, letting
  // a stick drag alone approach 20Hz+ and leave the link with far less headroom once the
  // dashboard's own polling is also streaming. Pace sends independent of round-trip time so
  // dragging never sends faster than this regardless of how quickly the board acks.
  const MIN_SEND_INTERVAL_MS = 100;   // 10Hz cap — plenty smooth for a hand on a virtual stick
  const lastSendAt = useRef(0);

  const trySend = () => {
    if (sending.current) return;
    const next = queued.current;
    if (!next) return;
    const wait = MIN_SEND_INTERVAL_MS - (Date.now() - lastSendAt.current);
    if (wait > 0) { setTimeout(trySend, wait); return; }
    queued.current = null;
    sending.current = true;
    lastSendAt.current = Date.now();
    p.onSetState(next).finally(() => {
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

  const dirBtn = (dir: keyof typeof dirs, icon: ReactNode) => (
    <button
      className="ghost sm icon-btn"
      style={{ background: dirs[dir] ? "#4ea1e0" : undefined, color: dirs[dir] ? "#000" : undefined }}
      onPointerDown={(e) => { e.preventDefault(); setDir(dir, true); }}
      onPointerUp={() => setDir(dir, false)}
      onPointerLeave={() => dirs[dir] && setDir(dir, false)}
    >
      {icon}
    </button>
  );

  if (!p.open) return null;

  return (
    <div
      ref={panelRef}
      style={{
        position: "fixed", bottom: 16, right: 16, width: 420, maxHeight: "80vh",
        transform: `translate(${pos.x}px, ${pos.y}px)`,
        background: "#1a1a1a", border: "2px solid #444", borderRadius: 8,
        padding: 16, boxShadow: "0 8px 24px rgba(0,0,0,0.5)", zIndex: 1000,
        overflowY: "auto", fontFamily: "inherit", color: "#fff",
      }}
    >
      <div
        onPointerDown={onHeaderPointerDown}
        onPointerMove={onHeaderPointerMove}
        onPointerUp={onHeaderPointerUp}
        onPointerLeave={onHeaderPointerUp}
        style={{
          display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 12,
          cursor: "move", touchAction: "none", userSelect: "none", margin: "-16px -16px 12px", padding: "16px 16px 8px",
        }}
      >
        <b>Virtual controller</b>
        <button className="ghost sm icon-btn" onClick={p.onClose} aria-label="Close"><X size={15} strokeWidth={2.25} /></button>
      </div>

      <div className="row gap" style={{ alignItems: "center", flexWrap: "wrap", gap: 16, marginBottom: 12 }}>
        {stickPad("l", "L stick")}
        {stickPad("r", "R stick")}
        <div className="row gap">
          {trigger("lt", "LT")}
          {trigger("rt", "RT")}
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 32px)", gridTemplateRows: "repeat(3, 32px)", gap: 2 }}>
          <span />{dirBtn("up", <ArrowUp size={15} strokeWidth={2.25} />)}<span />
          {dirBtn("left", <ArrowLeft size={15} strokeWidth={2.25} />)}<span />{dirBtn("right", <ArrowRight size={15} strokeWidth={2.25} />)}
          <span />{dirBtn("down", <ArrowDown size={15} strokeWidth={2.25} />)}<span />
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
      </div>

      <div className="muted sm" style={{ fontSize: "0.8em", lineHeight: "1.4", backgroundColor: "rgba(127,127,127,0.08)", padding: 8, borderRadius: 4 }}>
        <b>For live updates:</b> click <b>Start polling</b> in the Dashboard, then press/drag here to see changes.
      </div>
    </div>
  );
}
