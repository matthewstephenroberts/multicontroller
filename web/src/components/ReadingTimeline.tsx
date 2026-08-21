import { useState } from "react";
import type { ReactNode } from "react";
import type { Sensor } from "../types";
import { sensorValues, valueSelected } from "../types";

// Which value channels a sensor's timeline should chart — the selected ones (value_mask), or
// all of them if none are selected. Shared by Dashboard (to size the sensor's label row span)
// and this file (to render one chart per channel).
export function timelineChannels(sensor: Sensor): { idx: number; name: string }[] {
  const names = sensorValues(sensor);
  const mask = sensor.value_mask ?? 0xffff;
  const selected = names.map((_, i) => i).filter((i) => valueSelected(mask, i));
  const indices = selected.length > 0 ? selected : names.map((_, i) => i);
  return indices.map((idx) => ({ idx, name: names[idx] ?? `v${idx}` }));
}

// One channel's full-width sparkline + current value. Scaling per the dashboard's chart-scale
// mode: by default each chart auto-fits its own min/max over the recorded window (each value
// can have a wildly different range — r/g/b vs. a hue in degrees — so a shared scale across
// channels would flatten most of them to a near-invisible line), but auto-fit means the trace
// looks huge right after a reset and shrinks as the observed range grows. Passing `fixed`
// pins the y-axis to a stable range (the convert mode's declared min/max) so the trace's size
// stays comparable over time; the range is shown on the right so the pinning is visible.
// Width is driven entirely by the flex/grid parent (viewBox + preserveAspectRatio="none"), so
// every channel's chart — across every sensor — stretches to the same column width.
// `fixed` is the range actually applied (undefined = auto-fit). `onToggleScale` (with
// `canFix`) renders the scale indicator as a click-to-toggle so one channel can deviate from
// the dashboard-wide scale mode — e.g. as7341's raw Clear legitimately lives in the low
// thousands of a 0-40000 declared range, unreadably flat when pinned; one click flips just
// that chart back to auto-fit without giving up stable scales everywhere else.
export function ChannelChart({ name, values, timestamps, unit, extra, fixed, canFix, onToggleScale }: {
  name: string;
  values: number[];
  // Same length/index as `values` — entry.ts from the reading history. Optional so callers that
  // don't have timestamps handy (e.g. tests) don't have to fabricate them; the tooltip just omits
  // the "Ns ago" line when absent.
  timestamps?: number[];
  unit?: string;
  // Per-point extra context beyond the raw number — e.g. a colour sensor's swatch + classified
  // name for its "colour" channel. Returns null/undefined for channels with nothing extra to show.
  extra?: (value: number, index: number) => ReactNode;
  fixed?: { min: number; max: number };
  canFix?: boolean;
  onToggleScale?: () => void;
}) {
  // Hovered sample index, or null when the pointer is off the chart. Index into `values`/`points`
  // (not pixels) so the tooltip content and highlighted dot always agree with what was drawn.
  const [hover, setHover] = useState<number | null>(null);

  if (values.length === 0) {
    return <div className="timeline-placeholder muted">waiting for data… ({name})</div>;
  }

  const min = fixed ? fixed.min : Math.min(...values);
  const observedMax = fixed ? fixed.max : Math.max(...values);
  // Only pad the ceiling up to min+1 for a genuinely degenerate range (a perfectly flat series,
  // or a mode declaring max <= min) — Math.max(...values, min + 1) used to apply that padding
  // any time the real range was merely *smaller* than 1 unit (a 0/1 switch, a normalised 0-1
  // value, or anything that just hasn't drifted far yet), which floored the chart's ceiling at
  // min+1 regardless of the actual max and squashed genuine variation into a thin sliver at the
  // bottom instead of using the chart's full height.
  const max = observedMax > min ? observedMax : min + 1;
  const range = max - min || 1;

  const vbWidth = 600, vbHeight = 64;
  // Clamp into the drawable band — with a fixed range, live values can legitimately overshoot
  // the declared min/max (e.g. a raw count above the mode's nominal ceiling) and would
  // otherwise draw outside the SVG.
  const clamp01 = (t: number) => Math.max(0, Math.min(1, t));
  const points = values.map((v, i) => ({
    x: (i / Math.max(1, values.length - 1)) * vbWidth,
    y: vbHeight - clamp01((v - min) / range) * (vbHeight - 10) - 5,
  }));

  const pathD = points.length > 1
    ? `M ${points[0].x} ${points[0].y} ${points.map((p) => `L ${p.x} ${p.y}`).join(" ")}`
    : `M ${points[0]?.x ?? 0} ${points[0]?.y ?? vbHeight / 2}`;

  const current = values[values.length - 1];

  // Map a pointer's clientX to the nearest sample index — the SVG is stretched to 100% width via
  // preserveAspectRatio="none", so pixel space and viewBox space (0-600) only agree up to the
  // rendered/viewBox width ratio; go through the fraction rather than assuming 1:1.
  const indexAtClientX = (svg: SVGSVGElement, clientX: number): number => {
    const rect = svg.getBoundingClientRect();
    const frac = rect.width > 0 ? (clientX - rect.left) / rect.width : 0;
    const x = Math.max(0, Math.min(1, frac)) * vbWidth;
    let best = 0, bestDist = Infinity;
    for (let i = 0; i < points.length; i++) {
      const d = Math.abs(points[i].x - x);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
  };

  const hoverPoint = hover !== null ? points[hover] : null;
  const hoverValue = hover !== null ? values[hover] : null;
  const hoverTs = hover !== null ? timestamps?.[hover] : undefined;
  const hoverExtra = hover !== null && hoverValue !== null && extra ? extra(hoverValue, hover) : null;
  // Keep the tooltip's left edge from running off the chart at either end (translateX(-50%)
  // would otherwise clip against the parent's overflow at the first/last few samples).
  const hoverLeftPct = hoverPoint ? Math.max(6, Math.min(94, (hoverPoint.x / vbWidth) * 100)) : 0;

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
      <div style={{ display: "flex", justifyContent: "space-between", fontSize: "12px" }}>
        <span style={{ color: "var(--muted)" }}>{name}</span>
        <span>
          {canFix && onToggleScale ? (
            <button
              className="ghost sm"
              style={{ marginRight: 8, padding: "0 4px", fontSize: "11px" }}
              title={fixed
                ? "y-axis pinned to the mode's declared range — click to auto-fit this chart to its own data"
                : "y-axis auto-fits this chart's data — click to pin it to the mode's declared range"}
              onClick={onToggleScale}
            >
              {fixed ? `${fixed.min}–${fixed.max}` : "auto"}
            </button>
          ) : fixed ? (
            <span style={{ color: "var(--muted)", marginRight: 8 }}>
              {fixed.min}–{fixed.max}
            </span>
          ) : null}
          <span style={{ fontWeight: 600 }}>{Number.isFinite(current) ? current.toFixed(2) : "—"}</span>
        </span>
      </div>
      <div style={{ position: "relative" }}>
        <svg
          width="100%"
          height={64}
          viewBox={`0 0 ${vbWidth} ${vbHeight}`}
          preserveAspectRatio="none"
          style={{ border: "1px solid var(--line)", borderRadius: 6, background: "var(--panel-2)", display: "block", cursor: points.length > 1 ? "crosshair" : undefined }}
          onMouseMove={(e) => points.length > 1 && setHover(indexAtClientX(e.currentTarget, e.clientX))}
          onMouseLeave={() => setHover(null)}
        >
          <path d={pathD} stroke="var(--accent)" fill="none" strokeWidth={2} vectorEffect="non-scaling-stroke" />
          {points.length > 0 && (
            <circle cx={points[points.length - 1].x} cy={points[points.length - 1].y} r={3} fill="var(--accent)" />
          )}
          {hoverPoint && (
            <>
              <line
                x1={hoverPoint.x} y1={0} x2={hoverPoint.x} y2={vbHeight}
                stroke="var(--muted)" strokeWidth={1} strokeDasharray="2,2" vectorEffect="non-scaling-stroke"
              />
              <circle cx={hoverPoint.x} cy={hoverPoint.y} r={4} fill="var(--panel-2)" stroke="var(--accent)" strokeWidth={2} vectorEffect="non-scaling-stroke" />
            </>
          )}
        </svg>
        {hoverPoint && hoverValue !== null && (
          <div
            style={{
              position: "absolute",
              left: `${hoverLeftPct}%`,
              transform: "translate(-50%, -100%)",
              top: -6,
              background: "var(--panel)",
              border: "1px solid var(--line)",
              borderRadius: 6,
              padding: "4px 8px",
              fontSize: "11px",
              lineHeight: 1.4,
              whiteSpace: "nowrap",
              pointerEvents: "none",
              zIndex: 1,
              boxShadow: "0 2px 6px rgba(0,0,0,0.25)",
            }}
          >
            <div style={{ fontWeight: 600 }}>
              {Number.isFinite(hoverValue) ? hoverValue.toFixed(2) : "—"}{unit ? ` ${unit}` : ""}
            </div>
            {hoverExtra}
            {hoverTs !== undefined && (
              <div style={{ color: "var(--muted)" }}>{new Date(hoverTs).toLocaleTimeString()}</div>
            )}
          </div>
        )}
      </div>
    </div>
  );
}
