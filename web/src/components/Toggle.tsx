// Toggle.tsx — a pill switch styled like a LEGO stud sliding along a brick, used anywhere a
// plain on/off checkbox would otherwise appear (Settings' debug toggles, and available for
// reuse wherever else the app has a boolean control). Purely presentational — same on/off
// semantics as <input type="checkbox">, just one consistent, more polished control instead of
// the browser's native checkbox rendering differing slightly across the app.
import type { ReactNode } from "react";

interface Props {
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
  title?: string;
  label?: ReactNode;
}

export function Toggle({ checked, onChange, disabled, title, label }: Props) {
  return (
    <label className={`toggle-row${disabled ? " disabled" : ""}`} title={title}>
      <span
        className={`toggle${checked ? " on" : ""}`}
        role="switch"
        aria-checked={checked}
        tabIndex={disabled ? -1 : 0}
        onClick={() => !disabled && onChange(!checked)}
        onKeyDown={(e) => {
          if (disabled) return;
          if (e.key === " " || e.key === "Enter") { e.preventDefault(); onChange(!checked); }
        }}
      >
        <span className="toggle-stud" />
      </span>
      {label !== undefined && <span className="toggle-label">{label}</span>}
    </label>
  );
}
