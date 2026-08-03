import { useRef } from "react";
import type { ReactNode } from "react";
import type { LegoConfig } from "../types";
import { Toggle } from "./Toggle";
import { HelpTip } from "./HelpTip";
import { Wrench, Smile, Microscope, ToyBrick, RadioTower, Download, Upload } from "lucide-react";

// Device-wide debug/diagnostic toggles, split out of the LEGO tab (where "debug"/"events" used
// to live buried among unrelated emitter config) and the firmware (where the distance-sensor
// range diagnostics and BLE-HID report dumps used to be unconditional serial-log spam with no
// on/off switch at all — every board printed them regardless of whether anyone was debugging).
// All three toggles below apply instantly and persist on their own the moment you flip them —
// none of them need "Save to device", and none is presented as a sub-option of another.
interface Props {
  lego: LegoConfig;
  busy: string | null;
  advancedMode: boolean;
  onSetAdvancedMode: (advanced: boolean) => void;
  verboseDebug: boolean;
  onSetVerboseDebug: (enabled: boolean) => void;
  onSetLegoDebug: (events: boolean, debug: boolean) => void;
  onExportConfig: () => void;
  onImportConfig: (file: File) => void;
}

// One row: icon + name + one short line kids can read, a Toggle on the right. Every debug
// toggle in the app renders through this exact same shape — that consistency is the point.
function DebugRow({
  icon, name, hint, checked, onChange,
}: {
  icon: ReactNode; name: string; hint: string; checked: boolean; onChange: (v: boolean) => void;
}) {
  return (
    <div className="debug-row">
      <span className="debug-row-icon" aria-hidden="true">{icon}</span>
      <div className="debug-row-text">
        <b>{name}</b>
        <span className="muted sm">{hint}</span>
      </div>
      <Toggle checked={checked} onChange={onChange} />
    </div>
  );
}

export function SettingsForm(p: Props) {
  const fileInput = useRef<HTMLInputElement>(null);

  return (
    <section className="card">
      <div className="card-head">
        <h2>Settings</h2>
      </div>

      <div className="colour-section" style={{ marginBottom: 14 }}>
        <div className="colour-section-head">
          <h4 className="colour-section-title">
            {p.advancedMode ? "Grown-up view" : "Kid view"}
            <HelpTip>
              Kid view hides the tricky wiring stuff (bus, address, mux, pins) so you only see
              what you need to change. Grown-up view shows everything — useful for wiring up a
              new sensor or fixing a connection.
            </HelpTip>
          </h4>
          <span className="muted sm">
            {p.advancedMode ? "showing every setting, including wiring" : "showing only the settings a kid needs"}
          </span>
        </div>
        <div className="debug-rows">
          <DebugRow
            icon={p.advancedMode ? <Wrench size={18} strokeWidth={2.25} /> : <Smile size={18} strokeWidth={2.25} />}
            name={p.advancedMode ? "Grown-up mode" : "Kid mode"}
            hint={p.advancedMode ? "Turn off to go back to the simple kid view" : "Turn on to see and edit every wiring field"}
            checked={p.advancedMode}
            onChange={p.onSetAdvancedMode}
          />
        </div>
      </div>

      <div className="colour-section" style={{ marginBottom: 14 }}>
        <div className="colour-section-head">
          <h4 className="colour-section-title">
            Debug tools
            <HelpTip>
              These write extra notes to a special screen called the "serial console" — you
              won't see them here, only if a grown-up plugs the board into a computer and opens
              one. Safe to leave off; they're just for fixing tricky problems.
            </HelpTip>
          </h4>
          <span className="muted sm">turns on for the serial console (a computer plugged into the board)</span>
        </div>

        <div className="debug-rows">
          <DebugRow
            icon={<Microscope size={18} strokeWidth={2.25} />}
            name="Sensor debug"
            hint="Extra notes about distance sensors and game controllers"
            checked={p.verboseDebug}
            onChange={p.onSetVerboseDebug}
          />
          <DebugRow
            icon={<ToyBrick size={18} strokeWidth={2.25} />}
            name="LEGO events"
            hint="Notes each time the LEGO hub asks for data"
            checked={p.lego.events}
            onChange={(v) => p.onSetLegoDebug(v, p.lego.debug)}
          />
          <DebugRow
            icon={<RadioTower size={18} strokeWidth={2.25} />}
            name="LEGO byte trace"
            hint="Every single message sent to/from the LEGO hub — very chatty!"
            checked={p.lego.debug}
            onChange={(v) => p.onSetLegoDebug(p.lego.events, v)}
          />
        </div>
      </div>

      <div className="colour-section">
        <div className="colour-section-head">
          <h4 className="colour-section-title">
            Config backup
            <HelpTip>
              "Export" saves everything you've set up — every sensor, the display, the LEGO
              settings — into one file on your computer, like taking a photo of your setup.
              "Import" brings a saved photo back, in case you want to try something new and come
              back to it later.
            </HelpTip>
          </h4>
          <span className="muted sm">download / restore the full device config</span>
        </div>
        <div className="row gap">
          <button className="ghost sm icon-btn-label" disabled={!!p.busy} onClick={p.onExportConfig}>
            <Download size={14} strokeWidth={2.25} /> Export config…
          </button>
          <button
            className="ghost sm icon-btn-label"
            disabled={!!p.busy}
            onClick={() => fileInput.current?.click()}
          >
            <Upload size={14} strokeWidth={2.25} /> Import config…
          </button>
          <input
            ref={fileInput}
            type="file"
            accept="application/json,.json"
            style={{ display: "none" }}
            onChange={(e) => {
              const file = e.target.files?.[0];
              if (file) p.onImportConfig(file);
              e.target.value = "";   // allow re-selecting the same file next time
            }}
          />
        </div>
        <p className="muted sm" style={{ margin: "6px 0 0" }}>
          Export saves the board's current config (sensors, display, LEGO emitter, device name) as
          a JSON file. Import reads one back and asks for confirmation before overwriting the
          board's live config.
        </p>
      </div>
    </section>
  );
}
