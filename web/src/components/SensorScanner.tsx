import { useState } from "react";
import type { Discovered } from "../types";
import { BUS_DRIVER_TYPES } from "../types";
import { HelpTip } from "./HelpTip";

interface Props {
  discovered: Discovered[];
  knownIds: Set<number>;
  busy: string | null;
  config: any;
  onScan: () => void;
  onAdd: (d: Discovered) => void;
  onAddGamepad: () => void;
  onUseDisplay: (d: Discovered) => void;
}

function hex(n?: number): string {
  return n === undefined ? "—" : "0x" + n.toString(16).padStart(2, "0");
}

function AddBtn({ d, onAdd }: { d: Discovered; onAdd: (d: Discovered) => void }) {
  return (
    <button className="ghost sm" onClick={() => onAdd(d)}>
      + Add
    </button>
  );
}

// SPI/UART devices can't be identified by probing (no addressing/ID scheme), so the scan always
// reports "unknown" for them — instead of just adding as "generic", let the user pick which
// driver is actually wired up from that bus's named types before adding.
function UnknownRow({ d, onAdd }: { d: Discovered; onAdd: (d: Discovered) => void }) {
  const types = BUS_DRIVER_TYPES[d.bus as "spi" | "uart"] ?? ["generic"];
  const [type, setType] = useState(types[0]);
  return (
    <>
      <td>
        <select value={type} onChange={(e) => setType(e.target.value)}>
          {types.map((t) => (
            <option key={t} value={t}>{t}</option>
          ))}
        </select>
      </td>
      <td>
        <AddBtn d={{ ...d, guess: type }} onAdd={onAdd} />
      </td>
    </>
  );
}

export function SensorScanner(p: Props) {
  const i2c = p.discovered.filter((d) => d.bus === "i2c");
  const muxes = i2c
    .filter((d) => d.kind === "mux")
    .sort((a, b) => (a.addr ?? 0) - (b.addr ?? 0));
  const sensors = i2c.filter((d) => d.kind !== "mux" && !d.builtin);
  const direct = sensors.filter((d) => !d.mux_addr);
  const other = p.discovered.filter((d) => d.bus !== "i2c" && d.kind !== "display");
  const displays = p.discovered.filter((d) => d.kind === "display" || d.guess === "ssd1306");
  // Onboard chips wired to an internal bus with no discoverable address (e.g. the AtomS3R's
  // BMI270/BMM150) — always present rather than found, so they get their own "add" flow instead
  // of appearing in "Direct bus" as if scanned off the shared external bus.
  const builtinSensors = p.discovered.filter((d) => d.builtin && d.kind !== "display");

  const onMux = (mux: number) =>
    sensors
      .filter((d) => d.mux_addr === mux)
      .sort((a, b) => (a.channel ?? 0) - (b.channel ?? 0) || (a.addr ?? 0) - (b.addr ?? 0));

  return (
    <section className="card">
      <div className="card-head">
        <h2>
          Scan sensors
          <HelpTip>
            Press this button and your board will check every wire for something plugged in — a
            bit like turning on the lights to see what's in the room!
          </HelpTip>
        </h2>
        <button className="primary sm" disabled={!!p.busy} onClick={p.onScan}>
          {p.busy === "scanning" ? "Scanning buses…" : "Scan sensors"}
        </button>
      </div>

      {p.discovered.length === 0 ? (
        <p className="muted">
          Checks every wired bus this board has for something plugged in, plus any built-in
          onboard sensors. Results are grouped by where they were found.
        </p>
      ) : (
        <div className="scan-groups">
          {/* Displays (built-in SPI panel + any I2C OLED) */}
          {displays.length > 0 && (
            <div className="scan-group">
              <div className="scan-group-head">
                <span className="tag spi">display</span>
                <b>Displays</b>
                <span className="count">enable on the device</span>
              </div>
              <table className="grid">
                <tbody>
                  {displays.map((d, i) => (
                    <tr key={i}>
                      <td>{d.controller ?? d.guess}{d.builtin ? " (onboard)" : ""}</td>
                      <td>{d.bus === "i2c" ? hex(d.addr) : d.cs_index !== undefined ? `Chip select ${d.cs_index}` : d.bus}</td>
                      <td>
                        <button className="ghost sm" onClick={() => p.onUseDisplay(d)}>Use as display</button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}

          {/* Bluetooth game controller */}
          {!p.config.some((s: any) => s.type === "gamepad") && (
            <div className="scan-group">
              <div className="scan-group-head">
                <span className="tag i2c">bluetooth</span>
                <b>Game controller (BLE-HID)</b>
                <span className="count">add & pair</span>
              </div>
              <p className="muted sm">
                Hold your Xbox Series controller's pair button until it flashes, then click Add.
                The board will scan for it automatically after you save.
              </p>
              <table className="grid">
                <tbody>
                  <tr>
                    <td>Xbox Series (BLE-HID)</td>
                    <td>
                      <button className="ghost sm" onClick={p.onAddGamepad}>
                        + Add
                      </button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          )}

          {/* Onboard chips with no discoverable address (e.g. AtomS3R's BMI270/BMM150) */}
          {builtinSensors.length > 0 && (
            <div className="scan-group">
              <div className="scan-group-head">
                <span className="tag i2c">i2c</span>
                <b>Onboard sensors</b>
                <span className="count">built in, always present</span>
              </div>
              <table className="grid">
                <tbody>
                  {builtinSensors.map((d, i) => (
                    <tr key={i}>
                      <td>{d.guess} (built-in)</td>
                      <td>
                        <AddBtn d={d} onAdd={p.onAdd} />
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}

          {/* One group per detected mux */}
          {muxes.map((mux, mi) => {
            const devs = onMux(mux.addr ?? 0);
            return (
              <div className="scan-group" key={`mux${mi}`}>
                <div className="scan-group-head">
                  <span className="tag i2c">mux</span>
                  <b>TCA9548A @ {hex(mux.addr)}</b>
                  <span className="ok">detected ✓</span>
                  <span className="count">
                    {devs.length} sensor{devs.length !== 1 ? "s" : ""} · {mux.channels ?? 8} ch
                  </span>
                </div>
                {devs.length === 0 ? (
                  <p className="muted sm">No sensors on this mux's channels yet.</p>
                ) : (
                  <table className="grid">
                    <tbody>
                      {devs.map((d, i) => (
                        <tr key={i}>
                          <td className="chcell">ch {d.channel}</td>
                          <td>{hex(d.addr)}</td>
                          <td>{d.guess}</td>
                          <td>
                            <AddBtn d={d} onAdd={p.onAdd} />
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                )}
              </div>
            );
          })}

          {/* Direct (upstream / onboard) I2C devices */}
          {direct.length > 0 && (
            <div className="scan-group">
              <div className="scan-group-head">
                <span className="tag i2c">i2c</span>
                <b>Direct bus</b>
                <span className="count">onboard / no mux</span>
              </div>
              <table className="grid">
                <tbody>
                  {direct.map((d, i) => (
                    <tr key={i}>
                      <td>{hex(d.addr)}</td>
                      <td>{d.guess}</td>
                      <td>
                        <AddBtn d={d} onAdd={p.onAdd} />
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}

          {/* SPI / UART */}
          {other.length > 0 && (
            <div className="scan-group">
              <div className="scan-group-head">
                <b>Other buses</b>
              </div>
              <table className="grid">
                <tbody>
                  {other.map((d, i) => (
                    <tr key={i}>
                      <td>
                        <span className={`tag ${d.bus}`}>{d.bus}</span>{" "}
                        {d.bus === "spi" ? `Chip select ${d.cs_index}` : `port ${d.port}`}
                      </td>
                      {d.guess === "unknown" ? (
                        <UnknownRow d={d} onAdd={p.onAdd} />
                      ) : (
                        <>
                          <td>{d.guess}</td>
                          <td>
                            <AddBtn d={d} onAdd={p.onAdd} />
                          </td>
                        </>
                      )}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </div>
      )}
    </section>
  );
}
