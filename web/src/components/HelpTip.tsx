// HelpTip.tsx — a small round "?" button next to a card's title that expands a short,
// kid-friendly explanation inline (not a modal/portal — no z-index or mobile-tap issues, and
// it stays visible right next to whatever it's explaining). Deliberately light-touch: one
// button per card, one or two short sentences — not a tooltip on every field.
import { useState } from "react";
import type { ReactNode } from "react";
import { X } from "lucide-react";

export function HelpTip({ children }: { children: ReactNode }) {
  const [open, setOpen] = useState(false);
  return (
    <span className="help-tip">
      <button
        type="button"
        className="help-tip-btn"
        aria-expanded={open}
        aria-label="What does this do?"
        onClick={() => setOpen((v) => !v)}
      >
        ?
      </button>
      {open && (
        <span className="help-tip-box">
          {children}
          <button type="button" className="help-tip-close" onClick={() => setOpen(false)} aria-label="Close">
            <X size={12} strokeWidth={2.5} />
          </button>
        </span>
      )}
    </span>
  );
}
