import { useEffect, useMemo, useRef, useState } from "react";
import { BleClient } from "./ble/bleClient";
import { MockBleClient } from "./ble/mockBleClient";
import { api } from "./ble/protocol";
import type { ColourRef, Discovered, DisplayConfig, DisplayController, LegoConfig, Reading, Sensor, SensorActionError, SensorTimeSeries, VirtualGamepadState } from "./types";
import { defaultDisplay, defaultLego, newGamepadSensor, sensorFromDiscovered, POLLING_CAP_PRESETS } from "./types";
import { ConnectPanel } from "./components/ConnectPanel";
import { SensorScanner } from "./components/SensorScanner";
import { DisplayConfigForm, DISPLAY_PRESETS } from "./components/DisplayConfigForm";
import { SensorConfigForm } from "./components/SensorConfigForm";
import { LegoConfigForm } from "./components/LegoConfigForm";
import { GamepadVirtualModal } from "./components/GamepadVirtualModal";
import { Dashboard } from "./components/Dashboard";
import { Modal, ConfirmModal } from "./components/Modal";
import { SettingsForm } from "./components/SettingsForm";
import { GuideTab } from "./components/GuideTab";
import { Mascot } from "./components/Mascot";
import { BusyOverlay } from "./components/BusyOverlay";
import {
  BookOpen, Search, Puzzle, Monitor, ToyBrick, BarChart3, Settings,
  Pencil, X, FlaskConical, Gamepad2, Sun, Moon,
  type LucideIcon,
} from "lucide-react";

export type TabId = "guide" | "scan" | "sensors" | "display" | "lego" | "dashboard" | "settings";
type Theme = "light" | "dark";

// Lucide (lucide-react, ISC license — see THIRD_PARTY_NOTICES.md) instead of emoji: emoji glyphs
// render differently per OS/browser (different weight, colour, baseline), which was part of what
// made the tab bar/header feel inconsistent — an SVG icon set renders identically everywhere and
// inherits colour via currentColor, matching each tab's LEGO-colour theming automatically.
const TABS: { id: TabId; label: string; Icon: LucideIcon; colour: string }[] = [
  { id: "guide", label: "Guide", Icon: BookOpen, colour: "teal" },
  { id: "scan", label: "Scan", Icon: Search, colour: "yellow" },
  { id: "sensors", label: "Sensors", Icon: Puzzle, colour: "blue" },
  { id: "display", label: "Display", Icon: Monitor, colour: "azure" },
  { id: "lego", label: "LEGO", Icon: ToyBrick, colour: "red" },
  { id: "dashboard", label: "Dashboard", Icon: BarChart3, colour: "green" },
  { id: "settings", label: "Settings", Icon: Settings, colour: "azure" },
];

// A crisp little 2×2 LEGO brick (body + 4 round studs) for the header logo — clearer at small
// sizes than a text-shadow/box-shadow trick, and reads the same in light or dark mode.
function LogoBrick() {
  return (
    <svg className="logo-brick" width="28" height="28" viewBox="0 0 28 28" aria-hidden="true">
      <defs>
        <linearGradient id="brickBody" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor="#ef2d3f" />
          <stop offset="55%" stopColor="#d5192a" />
          <stop offset="100%" stopColor="#a81120" />
        </linearGradient>
        <radialGradient id="stud" cx="35%" cy="30%" r="75%">
          <stop offset="0%" stopColor="#f5707c" />
          <stop offset="55%" stopColor="#d5192a" />
          <stop offset="100%" stopColor="#b0141f" />
        </radialGradient>
      </defs>
      {/* studs sit fully above the body's top edge so none of the circle is clipped behind it */}
      <ellipse cx="9" cy="10.4" rx="4.6" ry="1.6" fill="#7a0d16" opacity="0.35" />
      <ellipse cx="19" cy="10.4" rx="4.6" ry="1.6" fill="#7a0d16" opacity="0.35" />
      <circle cx="9" cy="6.4" r="4.1" fill="url(#stud)" stroke="#8f0f19" strokeWidth="0.4" />
      <circle cx="19" cy="6.4" r="4.1" fill="url(#stud)" stroke="#8f0f19" strokeWidth="0.4" />
      <circle cx="7.6" cy="4.9" r="1.3" fill="#ffffff" opacity="0.55" />
      <circle cx="17.6" cy="4.9" r="1.3" fill="#ffffff" opacity="0.55" />
      <rect x="2" y="10" width="24" height="16" rx="3" fill="url(#brickBody)" stroke="#8f0f19" strokeWidth="0.5" />
      <rect x="2" y="10" width="24" height="5" rx="2.5" fill="#ffffff" opacity="0.2" />
      <rect x="2" y="22" width="24" height="4" rx="2" fill="#000000" opacity="0.18" />
    </svg>
  );
}

// Compact "device name" badge for the header: shows the board's configured BLE name (from
// config_store, not necessarily client.deviceName — that's the OS's cached name for the paired
// device, which only catches up to a rename after reconnecting) with a small pencil to rename it
// in place, rather than sending kids hunting through settings tabs for something this central.
function DeviceNameBadge({ name, busy, onRename }: { name: string; busy: boolean; onRename: (name: string) => void }) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(name);

  if (editing) {
    return (
      <form
        className="row gap"
        onSubmit={(e) => {
          e.preventDefault();
          const trimmed = draft.trim();
          if (trimmed && trimmed !== name) onRename(trimmed);
          setEditing(false);
        }}
      >
        <input
          autoFocus
          value={draft}
          maxLength={19}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => { if (e.key === "Escape") setEditing(false); }}
          style={{ width: 130 }}
        />
        <button className="ghost sm" type="submit" disabled={busy}>Save</button>
        <button className="ghost sm icon-btn" type="button" onClick={() => setEditing(false)} aria-label="Cancel rename">
          <X size={15} strokeWidth={2.25} />
        </button>
      </form>
    );
  }

  return (
    <button
      className="ghost sm"
      title="Rename this board's Bluetooth name"
      onClick={() => { setDraft(name); setEditing(true); }}
    >
      {name || "device"} <Pencil size={13} strokeWidth={2.25} className="inline-icon" />
    </button>
  );
}

export function App() {
  const clientRef = useRef<BleClient>();
  if (!clientRef.current) clientRef.current = new BleClient();
  // "Try demo mode" swaps the active client to a MockBleClient (same public surface as
  // BleClient — see mockBleClient.ts) so the whole app works with fake data and no Bluetooth
  // permission at all. Discarding demoClientRef.current on exit (see requestDisconnect) means
  // demo mode always restarts from the same curated default next time, rather than persisting.
  const demoClientRef = useRef<MockBleClient>();
  const [mode, setMode] = useState<"real" | "demo">("real");
  const client = mode === "demo" ? (demoClientRef.current ??= new MockBleClient()) : clientRef.current;

  const [connected, setConnected] = useState(false);
  const [config, setConfig] = useState<Sensor[]>([]);
  const [display, setDisplay] = useState<DisplayConfig>(defaultDisplay());
  const [lego, setLego] = useState<LegoConfig>(defaultLego());
  const [version, setVersion] = useState<number>(0);
  const [deviceName, setDeviceName] = useState<string>("");
  const [discovered, setDiscovered] = useState<Discovered[]>([]);
  const [readings, setReadings] = useState<Record<number, Reading>>({});
  const [matrix, setMatrix] = useState<string[] | null>(null); // 3×3 Light Matrix pixels from hub
  const [hid, setHid] = useState<{ connected: boolean; name: string }>({ connected: false, name: "" });
  // Board hardware capabilities reported by get_config (older firmware omits it — default
  // matches the original hardcoded assumption of 5 SPI CS lines + a display, so nothing
  // regresses on a board/firmware pair that predates this field).
  const [board, setBoard] = useState<{
    name?: string; spi_cs_count: number; has_uart?: boolean; has_display: boolean; tft_controller?: string;
    tft_cs?: number; tft_dc?: number; tft_rst?: number; tft_bl?: number;
  }>({ spi_cs_count: 5, has_uart: true, has_display: true });
  const [, setHidVirtual] = useState(false);
  const [gamepadModalOpen, setGamepadModalOpen] = useState(false);
  const [streaming, setStreaming] = useState(false);
  const [busy, setBusy] = useState<string | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const [tab, setTab] = useState<TabId>("sensors");
  const abortControllerRef = useRef<AbortController | null>(null);
  const [pollingCap, setPollingCap] = useState<number>(0);
  const [verboseDebug, setVerboseDebugState] = useState<boolean>(false);
  const [readingHistory, setReadingHistory] = useState<Record<number, SensorTimeSeries>>({});
  const [timelineOrder, setTimelineOrder] = useState<number[]>([]);
  const [theme, setTheme] = useState<Theme>(() => {
    const saved = localStorage.getItem("mc-theme");
    if (saved === "light" || saved === "dark") return saved;
    return matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  });
  // Kids/easy view (default) hides the wiring-level fields (bus, addr, mux, cs, pins) that a
  // child doesn't need to touch — grown-up/advanced view shows everything. Purely a local UI
  // preference, not device config, so it lives in localStorage rather than config_store.
  const [advancedMode, setAdvancedMode] = useState<boolean>(() => localStorage.getItem("mc-advanced-mode") === "1");

  // Last config/display/lego actually confirmed on the device (set whenever a getConfig
  // response is applied wholesale — connect, after Save, after Import, after Factory reset).
  // `config`/`display`/`lego` above are the live, user-editable copies; comparing them against
  // these snapshots is how we know there's something Save hasn't sent yet. null = not loaded
  // yet (nothing to compare against, so never "dirty").
  const [savedConfig, setSavedConfig] = useState<Sensor[] | null>(null);
  const [savedDisplay, setSavedDisplay] = useState<DisplayConfig | null>(null);
  const [savedLego, setSavedLego] = useState<LegoConfig | null>(null);
  const isDirty = useMemo(() => {
    if (!connected || savedConfig === null) return false;
    if (JSON.stringify(config) !== JSON.stringify(savedConfig)) return true;
    if (savedDisplay !== null && JSON.stringify(display) !== JSON.stringify(savedDisplay)) return true;
    if (savedLego !== null && JSON.stringify(lego) !== JSON.stringify(savedLego)) return true;
    return false;
  }, [connected, config, savedConfig, display, savedDisplay, lego, savedLego]);

  // Unsaved edits only live in React state — a disconnect (dead battery, out of range, browser
  // tab closed) before hitting Save loses them with no trace. Mirror them into localStorage
  // (debounced) so a reload/reconnect can offer to restore the draft instead of silently
  // starting over from whatever the board still has. Keyed by device name so a different board
  // doesn't inherit an unrelated draft.
  const draftKey = (name: string) => `mc-draft:${name || "device"}`;
  const [pendingDraft, setPendingDraft] = useState<{
    sensors: Sensor[]; display: DisplayConfig; lego: LegoConfig; savedAt: number;
  } | null>(null);

  useEffect(() => {
    if (!isDirty) return;
    const t = setTimeout(() => {
      localStorage.setItem(draftKey(deviceName), JSON.stringify({ sensors: config, display, lego, savedAt: Date.now() }));
    }, 800);
    return () => clearTimeout(t);
  }, [isDirty, config, display, lego, deviceName]);

  useEffect(() => {
    const handler = (e: BeforeUnloadEvent) => {
      if (isDirty) { e.preventDefault(); e.returnValue = ""; }
    };
    window.addEventListener("beforeunload", handler);
    return () => window.removeEventListener("beforeunload", handler);
  }, [isDirty]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem("mc-theme", theme);
  }, [theme]);

  useEffect(() => {
    localStorage.setItem("mc-advanced-mode", advancedMode ? "1" : "0");
  }, [advancedMode]);

  // The virtual controller modal is opened from the Dashboard's gamepad card — leaving that tab
  // still leaves it floating over whatever tab comes next, driving a sensor the user can no
  // longer even see. Close it the moment the Dashboard isn't the active tab, same as the
  // disconnect case (see onConnectionChange) already does for the same reason.
  useEffect(() => {
    if (tab !== "dashboard") setGamepadModalOpen(false);
  }, [tab]);

  // Keep the timeline order in sync with the enabled sensor set: preserve the user's chosen
  // order for sensors still around, append newly-enabled ones at the end, drop removed ones.
  useEffect(() => {
    const enabledIds = config.filter((s) => s.enabled).map((s) => s.id);
    setTimelineOrder((prev) => {
      const kept = prev.filter((id) => enabledIds.includes(id));
      const added = enabledIds.filter((id) => !kept.includes(id));
      const next = [...kept, ...added];
      const same = next.length === prev.length && next.every((id, i) => id === prev[i]);
      return same ? prev : next;
    });
  }, [config]);

  const pushLog = (m: string) => setLog((l) => [`${new Date().toLocaleTimeString()}  ${m}`, ...l].slice(0, 80));

  useEffect(() => {
    client.onReading = (r: Reading) => {
      // Replace the device timestamp with browser wall-clock for the *displayed* reading:
      // r.ts is milliseconds since the board booted, so "ago" maths against Date.now()
      // produced nonsense like "1783660951.6s ago". History entries keep the device ts —
      // frequency is computed from deltas, where the epoch doesn't matter (and the device
      // clock is the more accurate source for intervals).
      setReadings((prev) => ({ ...prev, [r.sensor]: { ...r, ts: Date.now() } }));
      setReadingHistory((prev) => {
        const hist = prev[r.sensor] || { id: r.sensor, entries: [], lastUpdateMs: 0, frequencyHz: 0 };
        const now = Date.now();
        const entries = [...hist.entries, { ts: r.ts, values: r.values, status: r.status }].slice(-500);
        const freq = entries.length >= 2
          ? (entries.length - 1) / ((entries[entries.length - 1].ts - entries[0].ts) / 1000)
          : 0;
        return { ...prev, [r.sensor]: { ...hist, entries, lastUpdateMs: now, frequencyHz: freq } };
      });
    };
    client.onMatrix = (pixels: string[]) => setMatrix(pixels);
    client.onHid = (connected: boolean, name: string) => setHid({ connected, name });
    client.onConnectionChange = (c) => {
      setConnected(c);
      if (!c) {
        setStreaming(false);
        setDiscovered([]);
        // The virtual controller modal drives a `hid_set_state` command straight at the device
        // — with no connection there's nothing to drive, and leaving it open just invites a kid
        // to keep tapping buttons into the void back at the connect screen.
        setGamepadModalOpen(false);
      }
    };
    client.onLog = pushLog;
  }, [client]);

  // Synchronous re-entry guard alongside the `busy` state: `busy` disables buttons, but it's
  // React state — the re-render that disables them lands asynchronously, so a rapid burst of
  // clicks (or a held Enter/Space auto-repeating on a focused button, ~130ms period) can all
  // start their own run() before the first setBusy takes effect. Observed as e.g. four
  // learn_colour commands hitting the firmware back-to-back from one Teach press. The ref
  // updates synchronously, so every call after the first bails immediately.
  const runningRef = useRef(false);

  // Last rejected teach/calibrate/reset, shown inline where it happened (see withActionError).
  const [actionError, setActionError] = useState<SensorActionError | null>(null);

  const run = async (label: string, fn: (signal?: AbortSignal) => Promise<void>, timeoutMs: number = 0) => {
    if (runningRef.current) return;
    runningRef.current = true;
    abortControllerRef.current = new AbortController();
    setBusy(label);
    try {
      let timeoutId: ReturnType<typeof setTimeout> | null = null;
      if (timeoutMs > 0) {
        timeoutId = setTimeout(() => {
          abortControllerRef.current?.abort();
        }, timeoutMs);
      }
      try {
        await fn(abortControllerRef.current.signal);
      } finally {
        if (timeoutId !== null) clearTimeout(timeoutId);
      }
    } catch (e) {
      const msg = (e as Error).message;
      if (msg !== 'The operation was aborted') {
        pushLog(`error: ${msg}`);
      }
    } finally {
      runningRef.current = false;
      abortControllerRef.current = null;
      setBusy(null);
    }
  };

  // Boards this origin has already been granted (Chrome getDevices()) — lets Connect show our
  // own React picker and reconnect with NO browser popup. A brand-new board still needs the
  // native chooser once: Web Bluetooth forbids sites from enumerating nearby devices, so no
  // custom UI can replace that first permission grant.
  const [rememberedPicker, setRememberedPicker] = useState<BluetoothDevice[] | null>(null);

  // Config import: a file is parsed and staged here so the user can review a summary before
  // it's actually applied (set_config overwrites the board's live config) — imports are not
  // versioned/undoable on the device, so this is the one chance to catch "wrong file" mistakes.
  const [pendingImport, setPendingImport] = useState<{
    sensors: Sensor[]; display: DisplayConfig; lego: LegoConfig; device_name?: string; fileName: string;
  } | null>(null);
  const [importError, setImportError] = useState<string | null>(null);

  // Takes the client explicitly (rather than closing over the outer `client` const) because
  // handleDemoConnect swaps the active client via `setMode`, a state update that only takes
  // effect on the *next* render — this function's own async continuation would otherwise still
  // see the pre-swap client for the rest of its execution.
  const loadConfigAfterConnect = async (c: BleClient) => {
    const cfg = await api.getConfig(c);
    const nextDisplay = cfg.display ? { ...defaultDisplay(), ...cfg.display } : display;
    const nextLego = cfg.lego ? { ...defaultLego(), ...cfg.lego } : lego;
    setConfig(cfg.sensors);
    setDisplay(nextDisplay);
    setLego(nextLego);
    if (cfg.device_name) setDeviceName(cfg.device_name);
    setVersion(cfg.version);
    // This IS the device's actual current state — anything the user edits from here is dirty
    // until the next successful Save/Import/Factory-reset.
    setSavedConfig(cfg.sensors);
    setSavedDisplay(nextDisplay);
    setSavedLego(nextLego);
    const capUs = cfg.polling_cap_us || (20 * 1000);
    setPollingCap(capUs > 0 ? 1000000 / capUs : 0);
    // The board auto-reconnects a paired gamepad on its own — adopt the snapshot so a
    // controller that connected before we did shows as connected immediately.
    if (cfg.hid) setHid(cfg.hid);
    if (cfg.board) setBoard(cfg.board);
    if (cfg.verbose_debug !== undefined) setVerboseDebugState(cfg.verbose_debug);

    // A leftover draft from a session that ended (disconnect/reload) before Save was hit —
    // offer to restore it rather than silently discarding whatever wasn't saved. Only bother
    // the user if it actually differs from what the device just reported; an identical or
    // stale-but-matching draft is cleaned up quietly.
    const raw = localStorage.getItem(draftKey(cfg.device_name || deviceName));
    if (raw) {
      try {
        const draft = JSON.parse(raw);
        const draftSnap = JSON.stringify({ sensors: draft.sensors, display: draft.display, lego: draft.lego });
        const liveSnap = JSON.stringify({ sensors: cfg.sensors, display: nextDisplay, lego: nextLego });
        if (draftSnap !== liveSnap) setPendingDraft(draft);
        else localStorage.removeItem(draftKey(cfg.device_name || deviceName));
      } catch {
        localStorage.removeItem(draftKey(cfg.device_name || deviceName));
      }
    }
  };

  const connectViaChooser = () =>
    run("connecting", async () => {
      await client.connect();
      await loadConfigAfterConnect(client);
    }, 15000);

  const connectRemembered = (dev: BluetoothDevice) => {
    setRememberedPicker(null);
    return run("connecting", async () => {
      await client.connectTo(dev);
      await loadConfigAfterConnect(client);
    }, 15000);
  };

  const handleCancelConnection = () => {
    abortControllerRef.current?.abort();
    pushLog("connection cancelled by user");
  };

  const handleConnect = async () => {
    if (mode === "demo") { demoClientRef.current = undefined; setMode("real"); }
    const remembered = await BleClient.rememberedDevices();
    if (remembered.length > 0) setRememberedPicker(remembered);
    else await connectViaChooser();
  };

  // Explicit local `demo` variable (not the outer `client` const) — `setMode` only swaps what
  // `client` resolves to on the *next* render, so the connect/loadConfig calls below must target
  // the new mock instance directly rather than risk touching the real BleClient mid-flight.
  const handleDemoConnect = () => {
    if (!demoClientRef.current) demoClientRef.current = new MockBleClient();
    const demo = demoClientRef.current;
    setMode("demo");
    return run("connecting", async () => {
      await demo.connect();
      await loadConfigAfterConnect(demo);
    });
  };

  const handleRenameDevice = (name: string) =>
    run("renaming", async () => {
      const res = await api.setDeviceName(client, name);
      setDeviceName(res.device_name);
      setVersion(res.version);
      pushLog(`device renamed to "${res.device_name}" — reconnect to see the new name in scans`);
    });

  const handleScan = () =>
    run("scanning", async () => {
      setDiscovered(await api.scan(client));
    });

  // A config save writes the full sensor list in one multi-chunk BLE request — while live
  // polling notifications keep streaming on the same connection, that traffic can saturate the
  // link (see ble_svc.c's READING_NOTIFY throttle comment) and stall or drop the save's
  // response, which then times out and looks like the board crashed. Pausing notifications
  // around the save (not the scheduler itself — sensors keep polling on the device) clears the
  // link for the save and resumes streaming right after.
  const withPausedStream = async (fn: () => Promise<void>) => {
    if (!streaming) return fn();
    await api.unsubscribe(client);
    try {
      await fn();
    } finally {
      await api.subscribe(client);
    }
  };

  // Round captured floats to 2 decimals before saving. get_config returns calibration/taught-ref
  // doubles at full precision (up to 17 significant digits each), and echoing those back in
  // set_config balloons the JSON — a few fully-taught palettes were enough to overflow the
  // firmware's RX reassembly buffer and time the save out. 0.005 of a count is far below sensor
  // noise and the classifier's match threshold, so nothing is lost.
  const round2 = (v: number) => Math.round(v * 100) / 100;
  const roundCalib = (s: Sensor) => ({
    calib: (s.calib ?? []).map(round2),
    colours: s.colours?.map((c) => ({ ...c, ref: c.ref.map(round2) })),
  });
  // Used by Import, which needs to actually push calibration onto a board — often a different
  // one, or one that's just been factory-reset — where "the device already has this" doesn't
  // apply.
  const compactSensors = (sensors: Sensor[]): Sensor[] =>
    sensors.map((s) => ({ ...s, ...roundCalib(s) }));
  // Used by Save. Every Teach/Calibrate/Reset action already writes straight to the device on
  // its own and patches the *same* fresh result into both `config` and `savedConfig`
  // (patchCalibColours below) — so savedConfig[i].calib/colours is a reliable "what the device
  // currently has" for that sensor, not just "what was last Saved". A save re-uploading that
  // data on top was pure redundant traffic: for a fully-taught multi-sensor config it was the
  // majority of the payload, and the actual cause of large saves timing out / dropping the BLE
  // connection mid-upload (a ~24KB config chunked over BLE is ~130 sequential round-trips at the
  // old chunk size — long enough for the OS/browser's own Bluetooth stack to give up and tear
  // the link down before the firmware ever saw a complete frame). Omitting calib/colours here
  // when they're unchanged tells the firmware (config_store_set_json) to just keep what it
  // already has instead of defaulting to empty. The one case that must still send them: a manual
  // "Edit" reference tweak in ColourPalette, which — unlike Teach/Calibrate/Reset — only touches
  // local state and has no live device-side counterpart until Save.
  const compactSensorsForSave = (sensors: Sensor[]): Sensor[] =>
    sensors.map((s) => {
      const rounded = roundCalib(s);
      const saved = savedConfig?.find((sv) => sv.id === s.id);
      const savedRounded = saved && roundCalib(saved);
      const unchanged = savedRounded &&
        JSON.stringify(rounded.calib) === JSON.stringify(savedRounded.calib) &&
        JSON.stringify(rounded.colours) === JSON.stringify(savedRounded.colours);
      if (unchanged) {
        const { calib, colours, ...rest } = s;
        return rest as Sensor;
      }
      return { ...s, ...rounded };
    });

  // The board applies a save to RAM and answers "ok" before its background flash write actually
  // runs (see config_store.c's persist_task) — a large config saved while sensors are polling
  // quickly enough to keep BLE-notify traffic busy can still fail that write on heap pressure
  // alone, invisibly to the "ok" response that already went out. get_config's persist_ok field
  // reports the *last* write's outcome, so check it again shortly after a save (past the async
  // write's retry window) rather than trusting the save response alone.
  const checkPersistOk = (label: string) => {
    setTimeout(() => {
      api.getConfig(client).then((cfg) => {
        if (cfg.persist_ok === false)
          pushLog(`warning: ${label} may not have reached flash (background save failed after retries) — try saving again`);
      }).catch(() => {});
    }, 1500);
  };

  const handleSave = () =>
    run("saving", () =>
      withPausedStream(async () => {
        const sent = compactSensorsForSave(config);
        let res: { version: number };
        try {
          res = await api.setConfig(client, sent, display, lego);
        } catch (e) {
          // The save itself was rejected/never reached the board — nothing changed there, so
          // "unsaved" correctly staying on is right, but say plainly that this is *why* rather
          // than leaving it looking like a silent no-op.
          pushLog(`config save failed: ${(e as Error).message}`);
          throw e;
        }
        setVersion(res.version);
        pushLog(`config saved (v${res.version})`);
        try {
          const cfg = await api.getConfig(client);
          const nextDisplay = cfg.display ? { ...defaultDisplay(), ...cfg.display } : display;
          const nextLego = cfg.lego ? { ...defaultLego(), ...cfg.lego } : lego;
          setConfig(cfg.sensors);
          setDisplay(nextDisplay);
          setLego(nextLego);
          if (cfg.device_name) setDeviceName(cfg.device_name);
          if (cfg.board) setBoard(cfg.board);
          setSavedConfig(cfg.sensors);
          setSavedDisplay(nextDisplay);
          setSavedLego(nextLego);
          localStorage.removeItem(draftKey(cfg.device_name || deviceName));
          checkPersistOk("config save");
        } catch (e) {
          // The save above DID succeed (res.version is the board's new version) — only the
          // confirmation re-read failed. Trust what was actually sent rather than leaving the
          // "unsaved" flag stuck forever over a read failure that has nothing to do with
          // whether the save landed.
          pushLog(`config saved (v${res.version}), but re-reading it back failed: ${(e as Error).message}`);
          // Baseline against the live `config`/`display`/`lego`, not the rounded `sent` payload
          // — compactSensors() only rounds calib/colour precision for the wire, the device's
          // actual stored values are equivalent up to that rounding, so comparing the unrounded
          // live state against itself is the right "no longer dirty" baseline, not a payload
          // that would spuriously differ from `config` by those same few rounded decimals.
          setSavedConfig(config);
          setSavedDisplay(display);
          setSavedLego(lego);
          localStorage.removeItem(draftKey(deviceName));
        }
      }),
    );

  // Wraps a calibrate/teach/reset call so a device-side rejection is shown inline on the exact
  // row/button that was clicked (via `actionError`), not only as a line in the Activity log —
  // a silently-refused Teach otherwise looks identical to "the UI didn't update". Any new
  // attempt clears the previous error; success leaves it cleared. Rethrows so run() still logs.
  const withActionError = async (
    err: Omit<SensorActionError, "message">,
    fn: () => Promise<void>,
  ) => {
    setActionError(null);
    try {
      await fn();
    } catch (e) {
      setActionError({ ...err, message: (e as Error).message });
      throw e;
    }
  };

  // Patch just one sensor's calib/colours into both `config` (live) and `savedConfig` (baseline)
  // from a calibrate/teach/reset response — these persist immediately server-side, so the
  // result IS the new saved state, not a live edit (mark it synced, or it'd read as "unsaved"
  // against the old baseline). This replaces what used to be a full get_config refetch after
  // every single click — the device already hands back the one sensor's fresh data directly, so
  // there's no need to re-fetch and replace the entire 9-sensor array just to see it.
  const patchCalibColours = (sensorId: number, patch: { calib?: number[]; colours?: ColourRef[] }) => {
    const apply = (arr: Sensor[]) => arr.map((s) => (s.id === sensorId ? { ...s, ...patch } : s));
    setConfig(apply);
    setSavedConfig((prev) => (prev ? apply(prev) : prev));
  };

  const handleCalibrate = (sensorId: number, point?: string) =>
    run("calibrating", () =>
      withPausedStream(() =>
        withActionError({ sensorId, action: "calibrate" }, async () => {
          const res = await api.calibrate(client, sensorId, point);
          pushLog(`calibrated sensor ${sensorId}${point ? ` (${point})` : ""} (v${res.version})`);
          patchCalibColours(sensorId, { calib: res.calib, colours: res.colours });
        }),
      ),
    );

  const handleLearnColour = (sensorId: number, name: string, outId: number) =>
    run("learning", () =>
      withPausedStream(() =>
        withActionError({ sensorId, action: "teach", colour: name }, async () => {
          const res = await api.learnColour(client, sensorId, name, outId);
          pushLog(`taught "${name}" (id ${outId}) on sensor ${sensorId} (v${res.version})`);
          patchCalibColours(sensorId, { calib: res.calib, colours: res.colours });
        }),
      ),
    );

  const handleResetColour = (sensorId: number, name: string) =>
    run("resetting", () =>
      withPausedStream(() =>
        withActionError({ sensorId, action: "reset", colour: name }, async () => {
          const res = await api.resetColour(client, sensorId, name);
          pushLog(`reset "${name}" on sensor ${sensorId} (v${res.version})`);
          patchCalibColours(sensorId, { calib: res.calib, colours: res.colours });
        }),
      ),
    );

  const handleResetSensor = (sensorId: number) =>
    run("resetting sensor", () =>
      withPausedStream(() =>
        withActionError({ sensorId, action: "reset_sensor" }, async () => {
          const res = await api.resetSensor(client, sensorId);
          pushLog(`reset sensor ${sensorId} calibration + taught colours (v${res.version})`);
          patchCalibColours(sensorId, { calib: res.calib ?? [], colours: res.colours ?? [] });
        }),
      ),
    );

  const handleFactoryReset = () =>
    run("factory-reset", () =>
      withPausedStream(async () => {
        const res = await api.factoryReset(client);
        pushLog(`factory reset — config erased (v${res.version})`);
        const cfg = await api.getConfig(client);
        const nextDisplay = cfg.display ? { ...defaultDisplay(), ...cfg.display } : display;
        const nextLego = cfg.lego ? { ...defaultLego(), ...cfg.lego } : lego;
        setConfig(cfg.sensors);
        setDisplay(nextDisplay);
        setLego(nextLego);
        if (cfg.device_name) setDeviceName(cfg.device_name);
        if (cfg.board) setBoard(cfg.board);
        setVersion(cfg.version);
        setSavedConfig(cfg.sensors);
        setSavedDisplay(nextDisplay);
        setSavedLego(nextLego);
        localStorage.removeItem(draftKey(cfg.device_name || deviceName));
      }),
    );

  // Export pulls a fresh copy straight from the board (not the possibly-unsaved in-progress
  // form state) so the downloaded file always matches what's actually persisted on the device.
  const handleExportConfig = () =>
    run("exporting", async () => {
      const cfg = await api.getConfig(client);
      const out = {
        version: cfg.version,
        sensors: compactSensors(cfg.sensors),
        display: cfg.display,
        lego: cfg.lego,
        device_name: cfg.device_name,
        polling_cap_us: cfg.polling_cap_us,
      };
      const blob = new Blob([JSON.stringify(out, null, 2)], { type: "application/json" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
      a.href = url;
      a.download = `${cfg.device_name || "multicontroller"}-config-${stamp}.json`;
      a.click();
      URL.revokeObjectURL(url);
      pushLog(`exported config (v${cfg.version})`);
    });

  const handleImportConfig = (file: File) => {
    setImportError(null);
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const parsed = JSON.parse(String(reader.result));
        if (!Array.isArray(parsed.sensors)) throw new Error("missing/invalid \"sensors\" array");
        setPendingImport({
          sensors: parsed.sensors,
          display: { ...defaultDisplay(), ...(parsed.display ?? {}) },
          lego: { ...defaultLego(), ...(parsed.lego ?? {}) },
          device_name: typeof parsed.device_name === "string" ? parsed.device_name : undefined,
          fileName: file.name,
        });
      } catch (e) {
        setImportError(`"${file.name}" isn't a valid config export: ${(e as Error).message}`);
      }
    };
    reader.onerror = () => setImportError(`couldn't read "${file.name}"`);
    reader.readAsText(file);
  };

  const handleConfirmImport = () => {
    const pending = pendingImport;
    if (!pending) return;
    run("importing", () =>
      withPausedStream(async () => {
        const res = await api.setConfig(client, compactSensors(pending.sensors), pending.display, pending.lego);
        if (pending.device_name) await api.setDeviceName(client, pending.device_name);
        pushLog(`imported config from "${pending.fileName}" (v${res.version})`);
        // Import fully replaces the config — any older unsaved draft is moot now.
        localStorage.removeItem(draftKey(pending.device_name || deviceName));
        await loadConfigAfterConnect(client);
        checkPersistOk("config import");
      }),
    );
  };

  const handleHidScan = () =>
    run("hid_scan", async () => {
      await api.hidScan(client);
      pushLog("scanning for a BLE-HID controller (put it in pairing mode)…");
    });

  const handleHidForget = () =>
    run("hid_forget", async () => {
      await api.hidForget(client);
      setHid({ connected: false, name: "" });
      pushLog("forgot the paired controller");
    });

  // Virtual on-screen controller. Toggling goes through `run` (infrequent, fine to disable
  // buttons briefly); pushing a stick/button state doesn't — it fires on every drag/click and
  // must never be blocked by `busy`, or the panel would feel unresponsive while streaming.
  // The modal auto-disables itself on close (including a forced close on disconnect — see
  // onConnectionChange) — with no connection there's no device to tell, so skip the request
  // rather than surfacing a spurious "not connected" error for something the user didn't do.
  const handleHidVirtualToggle = (enabled: boolean) => {
    if (!connected) { setHidVirtual(enabled); return; }
    return run("hid_virtual", async () => {
      await api.hidSetVirtual(client, enabled);
      setHidVirtual(enabled);
      pushLog(enabled ? "virtual controller enabled" : "virtual controller disabled");
    });
  };

  const handleHidSetState = (state: VirtualGamepadState) =>
    api.hidSetState(client, state)
      .then(() => undefined)
      .catch((e) => pushLog(`error: ${(e as Error).message}`));

  const handleStream = (on: boolean) =>
    run(on ? "starting" : "stopping", async () => {
      if (on) {
        await api.start(client);
        await api.subscribe(client);
      } else {
        // Unsubscribing alone only stops BLE notifications — the device's scheduler task keeps
        // polling every sensor at full rate in the background regardless, since nothing told it
        // to stop. That's needless bus/CPU load, and every one of those readings still gets
        // handed to send_framed's retry/backoff loop (a no-op once unsubscribed, but still queued
        // work) — the "keeps going" flood the unsubscribe-only version left behind. `stop` halts
        // the scheduler itself, matching the `start` this pairs with.
        await api.unsubscribe(client);
        await api.stop(client);
      }
      setStreaming(on);
    });

  const handleSetPollingCap = (hz: number) =>
    run("setting cap", async () => {
      await api.setPollingCap(client, hz);
      setPollingCap(hz);
      pushLog(`polling cap set to ${hz === 0 ? "auto" : hz + " Hz"}`);
    });

  const handleSetVerboseDebug = (enabled: boolean) =>
    run("setting debug", async () => {
      const res = await api.setVerboseDebug(client, enabled);
      setVerboseDebugState(res.verbose_debug);
      pushLog(`verbose sensor debug ${res.verbose_debug ? "on" : "off"}`);
    });

  // Same instant-apply shape as handleSetVerboseDebug — updates local `lego` AND `savedLego`
  // together, so flipping a debug toggle never marks the form dirty or waits for "Save to
  // device" (updating `lego` alone would make isDirty's lego-vs-savedLego comparison see a
  // false difference), matching how every other device debug toggle already behaves.
  const handleSetLegoDebug = (events: boolean, debug: boolean) =>
    run("setting debug", async () => {
      const res = await api.setLegoDebug(client, events, debug);
      setLego((cur) => ({ ...cur, events: res.events, debug: res.debug }));
      setSavedLego((cur) => (cur ? { ...cur, events: res.events, debug: res.debug } : cur));
      pushLog(`LEGO emitter debug: events ${res.events ? "on" : "off"}, byte trace ${res.debug ? "on" : "off"}`);
    });

  const handleResetHistory = () => {
    setReadingHistory({});
    pushLog("cleared live polling timeline data");
  };

  // Shared by useDisplay() (from a scan result) and resetDisplayToBoardDefaults() (from the
  // board capability object, no scan needed) — applies a controller's geometry preset plus,
  // when the actual wired pins are known (the onboard panel only — an external SPI display
  // can't self-report), those pins too. Not just controller+geometry: a stale saved config
  // (e.g. one saved before this board's earlier CS/DC swap fix) needs its pins reset as well,
  // or switching controller alone leaves it pointed at the wrong GPIOs.
  const applyDisplayDefaults = (
    controller: DisplayController,
    opts: { addr?: number; cs?: number; dc?: number; rst?: number; bl?: number },
  ) => {
    const geom = DISPLAY_PRESETS[controller];
    const addr = controller === "ssd1306" ? { addr: opts.addr ?? 0x3c } : {};
    setDisplay((cur) => {
      const pins = opts.cs !== undefined
        ? { cs: opts.cs, dc: opts.dc ?? cur.dc, rst: opts.rst ?? cur.rst, bl: opts.bl ?? cur.bl }
        : {};
      return { ...cur, enabled: true, controller, ...geom, ...addr, ...pins };
    });
    pushLog(`display set to ${controller} — review settings and Save`);
  };

  const useDisplay = (d: Discovered) => {
    // Trust whatever the board itself reported (scan's "controller", or "guess" for the I2C
    // OLED case, which isn't a distinct SPI panel entry) — this used to hardcode a 2-way
    // ili9341/ssd1306 whitelist that silently defaulted every other controller (including a
    // correctly-reported "gc9107") to st7789, regardless of what the board actually has.
    const reported = d.controller ?? (d.guess === "ssd1306" ? "ssd1306" : undefined);
    const controller: DisplayController =
      reported && reported in DISPLAY_PRESETS ? (reported as DisplayController) : "st7789";
    applyDisplayDefaults(controller, d.builtin
      ? { addr: d.addr, cs: d.cs, dc: d.dc, rst: d.rst, bl: d.bl }
      : { addr: d.addr });
  };

  // Reset the Display tab straight to what this connected board's firmware actually reports as
  // wired (board.tft_controller/tft_cs/tft_dc/tft_rst/tft_bl) — available right away at connect
  // time via get_config's board capability object, no need to go run a scan first. This is the
  // recovery path when a bad/stale display config is already saved: switching controller alone
  // in the dropdown doesn't touch cs/dc/rst/bl, so without this there'd be no way back to the
  // board's real pins short of a factory reset.
  const resetDisplayToBoardDefaults = () => {
    if (!board.has_display || !board.tft_controller) {
      pushLog("this board has no onboard display to reset to");
      return;
    }
    const controller: DisplayController =
      board.tft_controller in DISPLAY_PRESETS ? (board.tft_controller as DisplayController) : "st7789";
    applyDisplayDefaults(controller, { cs: board.tft_cs, dc: board.tft_dc, rst: board.tft_rst, bl: board.tft_bl });
  };

  const knownIds = useMemo(() => new Set(config.map((s) => s.id)), [config]);

  // A draft was already mirrored to localStorage while dirty (see the autosave effect above),
  // so a disconnect here doesn't actually lose the edits — but confirm anyway, since the user
  // has no way to know that without being told, and "my changes vanished" is the exact
  // complaint this whole feature exists to prevent.
  const [pendingDisconnectConfirm, setPendingDisconnectConfirm] = useState(false);
  // Discarding the mock instance here (rather than just disconnecting it) is what makes demo
  // mode "reset every time" — the next "Try demo mode" click builds a fresh MockBleClient from
  // buildDemoDefaults() instead of picking up wherever this session left off.
  const doDisconnect = () => {
    client.disconnect();
    if (mode === "demo") { demoClientRef.current = undefined; setMode("real"); }
  };
  const requestDisconnect = () => {
    if (isDirty) setPendingDisconnectConfirm(true);
    else doDisconnect();
  };

  return (
    <div className="app-shell">
      <header className="app-header">
        <div>
          <h1><LogoBrick />Multi<span className="accent">Controller</span></h1>
          <p className="sub">ESP32-S3 dynamic sensor hub — configure over Bluetooth LE</p>
        </div>
        <div className="header-right">
          {connected && (
            <div className="row gap" title={`config v${version}`}>
              <span className="dot on" />
              {mode === "demo" && (
                <span
                  className="tag"
                  style={{ background: "var(--lego-teal)", color: "#fff", fontWeight: 700 }}
                  title="Fake sensors, no real board — Disconnect to leave demo mode"
                >
                  <FlaskConical size={12} strokeWidth={2.5} className="inline-icon" /> DEMO
                </span>
              )}
              <DeviceNameBadge name={deviceName} busy={!!busy} onRename={handleRenameDevice} />
              {/* Only shown when it actually adds information — board.name (hardware this
                  firmware was built for) and deviceName (user-chosen BLE name, editable above)
                  usually differ, but coincide on an unrenamed/demo board, where showing both
                  back-to-back was just the same text twice. */}
              {board.name && board.name !== deviceName && (
                <span className="muted sm" title="Board this firmware was built for (board_config.h's BOARD_NAME)">
                  {board.name}
                </span>
              )}
              <span
                className="row gap"
                style={{ gap: 6 }}
                title={hid.connected ? `controller connected: ${hid.name || "unknown"}` : "no controller connected"}
              >
                <span className={`dot ${hid.connected ? "on" : "off"}`} />
                <Gamepad2 size={16} strokeWidth={2.25} className="muted" aria-hidden="true" />
              </span>
              {["sensors", "display", "lego", "settings"].includes(tab) && (
                <button className="primary sm" disabled={!!busy} onClick={handleSave}>
                  {busy === "saving" ? "Saving…" : isDirty ? "Save to device ●" : "Save to device"}
                </button>
              )}
              {isDirty && (
                <span className="muted sm" title="Changes here haven't been sent to the board yet — Save to device, or they'll be lost on disconnect (a local draft is kept so you can restore it if you don't).">
                  unsaved
                </span>
              )}
              <button
                className={streaming ? "ghost sm" : "primary sm"}
                disabled={!!busy}
                onClick={() => handleStream(!streaming)}
                title={streaming ? "Stop polling sensors" : "Start polling sensors"}
              >
                {streaming ? "● Polling" : "Start polling"}
              </button>
              <select
                className="polling-cap-select"
                value={pollingCap}
                disabled={!!busy}
                onChange={(e) => handleSetPollingCap(Number(e.target.value))}
                title="BLE notification rate cap"
              >
                {POLLING_CAP_PRESETS.map((p) => (
                  <option key={p.hz} value={p.hz}>{p.label}</option>
                ))}
              </select>
              <button className="ghost sm" onClick={requestDisconnect}>Disconnect</button>
            </div>
          )}
          <button
            className="theme-toggle"
            title={theme === "dark" ? "Switch to light mode" : "Switch to dark mode"}
            onClick={() => setTheme((t) => (t === "dark" ? "light" : "dark"))}
          >
            {theme === "dark" ? <Sun size={18} strokeWidth={2} /> : <Moon size={18} strokeWidth={2} />}
          </button>
        </div>
      </header>

      {connected && (
        <nav className="tabbar">
          {TABS.map((t) => (
            <button
              key={t.id}
              className={`tab${tab === t.id ? " active" : ""}`}
              data-colour={t.colour}
              onClick={() => setTab(t.id)}
            >
              <t.Icon className="tab-icon" size={17} strokeWidth={2.25} aria-hidden="true" /> {t.label}
            </button>
          ))}
        </nav>
      )}

      <main className="app-main">
        {!connected ? (
          <div className="welcome">
            <div className="mascot-wrap"><Mascot mood="happy" size={110} /></div>
            <h2>Let's build something! 🧱</h2>
            <p>
              Connect to your MultiController board over Bluetooth to scan for sensors, pick what
              a LEGO hub sees, and watch everything live.
            </p>
            <div style={{ width: "100%", maxWidth: 420 }}>
              <ConnectPanel
                supported={BleClient.supported}
                connected={connected}
                deviceName={client.deviceName}
                version={version}
                busy={busy}
                onConnect={handleConnect}
                onDisconnect={requestDisconnect}
                onTryDemo={handleDemoConnect}
                onCancel={handleCancelConnection}
              />
            </div>
          </div>
        ) : (
          /* data-colour mirrors the active tab's own colour (see TABS above) onto --panel-accent
             (styles.css) — every informational icon inside the panel (Ruler, Target, Timer,
             Palette, ...) picks it up by default instead of a flat neutral grey, so the tab
             bar's colour identity actually carries into the content it's showing rather than
             stopping at the tab pill itself. Warning icons stay on --warn regardless (see
             .warn's own color rule) — colour-coding "this needs attention" against whichever
             tab happens to be open would undermine the one colour that's supposed to mean
             something specific everywhere. */
          <div className="panel-scroll" data-colour={TABS.find((t) => t.id === tab)?.colour}>
            {tab === "guide" && <GuideTab onGoTo={(t) => setTab(t)} />}

            {tab === "scan" && (
              <SensorScanner
                discovered={discovered}
                knownIds={knownIds}
                busy={busy}
                config={config}
                onScan={handleScan}
                onAdd={(d) => setConfig((c) => [...c, sensorFromDiscovered(d, c)])}
                onAddGamepad={() => setConfig((c) => [...c, newGamepadSensor(c)])}
                onUseDisplay={useDisplay}
              />
            )}

            {tab === "sensors" && (
              <SensorConfigForm
                config={config}
                spiCsCount={board.spi_cs_count}
                hasUart={board.has_uart}
                displayEnabled={display.enabled}
                paged={display.mode === "paged"}
                advanced={advancedMode}
                busy={busy}
                readings={readings}
                actionError={actionError}
                onChange={setConfig}
                onSave={handleSave}
                onCalibrate={handleCalibrate}
                onLearnColour={handleLearnColour}
                onResetColour={handleResetColour}
                onResetSensor={handleResetSensor}
                onFactoryReset={handleFactoryReset}
                hid={hid}
                onHidScan={handleHidScan}
                onHidForget={handleHidForget}
              />
            )}

            {tab === "display" && (
              <DisplayConfigForm
                display={display}
                busy={busy}
                board={board}
                advanced={advancedMode}
                onChange={setDisplay}
                onSave={handleSave}
                onResetToBoardDefaults={resetDisplayToBoardDefaults}
              />
            )}

            {tab === "lego" && (
              <LegoConfigForm
                lego={lego}
                sensors={config}
                readings={readings}
                streaming={streaming}
                matrix={matrix}
                advanced={advancedMode}
                board={board}
                busy={busy}
                onChange={setLego}
                onSave={handleSave}
              />
            )}

            {tab === "dashboard" && (
              <Dashboard
                config={config}
                readings={readings}
                readingHistory={readingHistory}
                timelineOrder={timelineOrder}
                streaming={streaming}
                busy={busy}
                gamepadModalOpen={gamepadModalOpen}
                onToggleStream={handleStream}
                onReorder={setTimelineOrder}
                onResetHistory={handleResetHistory}
                onToggleGamepadModal={setGamepadModalOpen}
              />
            )}

            {tab === "settings" && (
              <SettingsForm
                lego={lego}
                busy={busy}
                advancedMode={advancedMode}
                onSetAdvancedMode={setAdvancedMode}
                verboseDebug={verboseDebug}
                onSetVerboseDebug={handleSetVerboseDebug}
                onSetLegoDebug={handleSetLegoDebug}
                onExportConfig={handleExportConfig}
                onImportConfig={handleImportConfig}
              />
            )}
          </div>
        )}
      </main>

      <footer className="app-footer">
        <details className="log">
          <summary>Activity log</summary>
          <pre>{log.join("\n")}</pre>
        </details>
      </footer>

      {rememberedPicker && (
        <Modal
          title="Connect to a board"
          onClose={() => setRememberedPicker(null)}
          actions={
            <>
              {busy === "connecting" && (
                <button className="danger sm" onClick={handleCancelConnection}>Cancel connection</button>
              )}
              <button className="ghost sm" onClick={() => setRememberedPicker(null)}>Close</button>
            </>
          }
        >
          <p className="muted sm" style={{ marginTop: 0 }}>
            Boards you've connected before — no browser popup needed. Make sure the board is
            powered and advertising.
          </p>
          {rememberedPicker.map((dev, di) => (
            <div key={dev.id} className="row gap" style={{ justifyContent: "space-between", padding: "6px 0", borderBottom: "1px solid var(--line)" }}>
              <b>{dev.name ?? "(unnamed board)"}</b>
              <span className="row gap">
                <button className="ghost sm" disabled={!!busy} onClick={() => connectRemembered(dev)}>Connect</button>
                <button
                  className="ghost sm danger"
                  title="Remove this board from the remembered list (you'll use the browser chooser next time)"
                  onClick={async () => {
                    await BleClient.forgetDevice(dev);
                    setRememberedPicker((cur) => cur?.filter((_, i) => i !== di) ?? null);
                  }}
                >
                  Forget
                </button>
              </span>
            </div>
          ))}
          <button
            className="ghost sm"
            style={{ marginTop: 10 }}
            disabled={!!busy}
            title="Opens the browser's device chooser — required once for any board this browser hasn't been granted before"
            onClick={() => { setRememberedPicker(null); void connectViaChooser(); }}
          >
            + choose another board… (browser chooser)
          </button>
        </Modal>
      )}

      {pendingImport && (
        <ConfirmModal
          title="Import config"
          confirmLabel="Import & overwrite"
          danger
          onClose={() => setPendingImport(null)}
          onConfirm={handleConfirmImport}
          message={
            <>
              Importing <b>{pendingImport.fileName}</b> will overwrite the board's current config
              with:
              <ul style={{ margin: "8px 0 0", paddingLeft: 18 }}>
                <li>{pendingImport.sensors.length} sensor{pendingImport.sensors.length === 1 ? "" : "s"}</li>
                <li>display: {pendingImport.display.enabled ? `on, ${pendingImport.display.mode}` : "off"}</li>
                <li>device name: {pendingImport.device_name || "(unchanged)"}</li>
              </ul>
              This can't be undone from the web app — export the board's current config first if
              you want to keep it.
            </>
          }
        />
      )}

      {importError && (
        <ConfirmModal
          title="Import failed"
          confirmLabel="OK"
          onClose={() => setImportError(null)}
          onConfirm={() => setImportError(null)}
          message={importError}
        />
      )}

      {pendingDraft && (
        <ConfirmModal
          title="Restore unsaved changes?"
          confirmLabel="Restore"
          onClose={() => { localStorage.removeItem(draftKey(deviceName)); setPendingDraft(null); }}
          onConfirm={() => {
            setConfig(pendingDraft.sensors);
            setDisplay(pendingDraft.display);
            setLego(pendingDraft.lego);
            setPendingDraft(null);
            pushLog("restored unsaved changes from a previous session — Save to device to keep them");
          }}
          message={
            <>
              This board has edits from {new Date(pendingDraft.savedAt).toLocaleString()} that were
              never saved before disconnecting. Restore them into the form now (you'll still need
              to hit <b>Save to device</b>), or discard them and start from what's actually on the
              board.
            </>
          }
        />
      )}

      {pendingDisconnectConfirm && (
        <ConfirmModal
          title="Disconnect with unsaved changes?"
          confirmLabel="Disconnect anyway"
          danger
          onClose={() => setPendingDisconnectConfirm(false)}
          onConfirm={() => { setPendingDisconnectConfirm(false); doDisconnect(); }}
          message={
            <>
              You have unsaved sensor/display/LEGO changes. They're kept as a local draft in this
              browser and this device will offer to restore them next time you reconnect to this
              board — but they won't reach the board itself unless you <b>Save to device</b> first.
            </>
          }
        />
      )}

      <GamepadVirtualModal
        open={gamepadModalOpen}
        onClose={() => setGamepadModalOpen(false)}
        onToggleEnabled={handleHidVirtualToggle}
        onSetState={handleHidSetState}
      />

      {/* Blocking overlay only for the two payload-heavy round trips that already pause live
          polling via withPausedStream (save/import) — the multi-KB transfer can take up to
          setConfig's 20s timeout, and without this the panel just looks frozen for that whole
          window. Lighter actions (calibrate, rename, etc.) already have their own inline
          "…ing" button state and don't need the full-screen treatment. */}
      {busy === "saving" && <BusyOverlay label="Saving to device" />}
      {busy === "importing" && <BusyOverlay label="Importing config" />}
    </div>
  );
}
