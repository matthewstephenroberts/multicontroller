// Gamepad pairing/scanning UI for Sensor config page
import { Gamepad2 } from "lucide-react";

interface HidStatus {
  connected: boolean;
  name: string;
}

interface Props {
  hid: HidStatus;
  busy: string | null;
  onScan: () => void;
  onForget: () => void;
}

export function GamepadPairingSection(p: Props) {
  return (
    <div style={{ borderTop: "1px solid rgba(127,127,127,0.2)", paddingTop: 12, marginTop: 12 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 12 }}>
        <Gamepad2 size={19} strokeWidth={2.25} className="muted" aria-hidden="true" />
        <div>
          <b>Game controller (BLE-HID)</b>
          <div className="muted sm">
            {p.hid.connected ? `✓ Connected: ${p.hid.name}` : "Not connected"}
          </div>
        </div>
      </div>

      <div className="row gap">
        <button
          className="primary sm"
          disabled={!!p.busy}
          title="Hold the controller's pair button until it flashes, then click Scan to connect"
          onClick={p.onScan}
        >
          {p.busy === "hid_scan" ? "Scanning…" : "Pair / Scan"}
        </button>
        {p.hid.connected && (
          <button
            className="ghost sm danger"
            disabled={!!p.busy}
            title="Unpair: erase the stored bond"
            onClick={p.onForget}
          >
            Forget
          </button>
        )}
      </div>

      <p className="muted sm" style={{ marginTop: 8 }}>
        Connect an <b>Xbox Series</b> controller over Bluetooth LE. ESP32-S3 is BLE-only: PS4/PS5
        won't connect.
      </p>
    </div>
  );
}
