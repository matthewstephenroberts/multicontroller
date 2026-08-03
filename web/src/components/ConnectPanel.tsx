import { FlaskConical } from "lucide-react";

interface Props {
  supported: boolean;
  connected: boolean;
  deviceName: string;
  version: number;
  busy: string | null;
  onConnect: () => void;
  onDisconnect: () => void;
  onTryDemo: () => void;
  onCancel?: () => void;
}

// Every browser on iOS/iPadOS — including "Chrome" and "Edge" for iOS — is required by Apple to
// use the system WebKit engine under the hood, and WebKit has never implemented Web Bluetooth.
// So on iOS this isn't fixable by switching apps the way it would be on desktop/Android; worth
// saying so explicitly rather than just "use Chrome" (which the user may already be doing).
const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);

export function ConnectPanel(p: Props) {
  return (
    <section className="card">
      <div className="card-head">
        <h2>Device</h2>
        <span className={`dot ${p.connected ? "on" : "off"}`} />
      </div>

      {!p.supported && (
        <p className="warn">
          {isIOS ? (
            <>
              Web Bluetooth isn't available on iPhone/iPad in <b>any</b> browser — Apple requires
              every iOS browser (including Chrome and Edge for iOS) to use its WebKit engine,
              which doesn't support it. This isn't fixable by switching apps here; use{" "}
              <b>Chrome</b> or <b>Edge</b> on a desktop computer, or <b>Chrome on Android</b>, instead.
            </>
          ) : (
            <>Web Bluetooth isn't available in this browser. Use <b>Chrome</b> or <b>Edge</b> on desktop, or Chrome on Android.</>
          )}
        </p>
      )}

      <div className="row" style={{ flexWrap: "wrap", justifyContent: "center" }}>
        {!p.connected ? (
          <>
            <button className="primary" disabled={!p.supported || !!p.busy} onClick={p.onConnect}>
              {p.busy === "connecting" ? "Scanning…" : "Scan for device"}
            </button>
            {p.busy === "connecting" && (
              <button className="danger sm" onClick={p.onCancel}>
                Cancel
              </button>
            )}
            <button className="ghost icon-btn-label" disabled={!!p.busy} onClick={p.onTryDemo}>
              <FlaskConical size={15} strokeWidth={2.25} className="inline-icon" /> Try demo mode
            </button>
          </>
        ) : (
          <button className="ghost" onClick={p.onDisconnect}>Disconnect</button>
        )}
      </div>
      <div className="meta" style={{ textAlign: "center", marginTop: 8 }}>
        {p.connected ? (
          <>
            Connected to <b>{p.deviceName || "device"}</b> · config v{p.version}
          </>
        ) : (
          <span className="muted">Not connected</span>
        )}
      </div>
      {!p.connected && (
        <p className="muted sm" style={{ margin: "6px 0 0" }}>
          Explore with fake sensors — no board or Bluetooth needed.
        </p>
      )}
    </section>
  );
}
