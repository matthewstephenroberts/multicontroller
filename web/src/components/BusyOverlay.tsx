import { Mascot } from "./Mascot";

// Full-screen, non-dismissable overlay shown while a heavy BLE round trip is in flight (a
// config Save/Import — see App.tsx's withPausedStream, which already unsubscribes live
// polling for the same window and resubscribes after). `disabled={!!busy}` on individual
// buttons already stops the obvious re-entry, but this is the visible, can't-miss-it version:
// a large multi-sensor save can take up to the setConfig timeout (20s, see protocol.ts) to
// land, and without this the panel just sits there looking unresponsive with no indication
// anything is happening. No onClose/Escape handling on purpose — unlike Modal.tsx's dialogs,
// there's nothing sensible to cancel back to mid-write.
export function BusyOverlay({ label }: { label: string }) {
  return (
    <div className="busy-overlay" role="alert" aria-busy="true">
      <div className="busy-card">
        <Mascot mood="working" size={96} className="busy-mascot" />
        <div className="busy-label">
          <span>{label}</span>
          <span className="dot">.</span>
          <span className="dot">.</span>
          <span className="dot">.</span>
        </div>
        <div className="busy-hint">Stay on this page — don't disconnect or close the tab.</div>
      </div>
    </div>
  );
}
