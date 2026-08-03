// mockBleClient.ts — an in-browser fake device for "Try demo mode": drop-in for BleClient so the
// entire app (Scan, Sensors, Display, LEGO emitter, Dashboard, Controller) works with no real
// hardware and no Bluetooth permission at all.
//
// Must `extend BleClient` rather than just duck-type its public members: BleClient has private
// fields (device/rx/tx/...), and TypeScript only allows a class to be assignable to a
// BleClient-typed parameter (every call site in ble/protocol.ts and App.tsx) if it's the same
// class or a subclass. Every public method/getter that touches real GATT is overridden below;
// nothing here ever calls the base class's implementations.
import { BleClient } from "./bleClient";
import type {
  ColourRef, Discovered, DisplayConfig, LegoConfig, Reading, Sensor, VirtualGamepadState,
} from "../types";
import {
  defaultDisplay, defaultLego, isColourSensor, LEGO_TARGET_RGBI, newGamepadSensor,
  scaleFromRange, sensorFromDiscovered, sensorValueMeta, sensorValues, SPIKE_COLOURS,
} from "../types";

const READING_TICK_MS = 200;
// Real hardware's set_config is the one command slow enough to actually show BusyOverlay's
// animation (large multi-sensor payload, chunked over BLE — see protocol.ts's setConfig
// comment) — the mock otherwise resolves every command same-tick, so demo mode's Save button
// used to flash the overlay for a single frame, if at all. This isn't trying to be a realistic
// transfer-time simulation, just long enough (with real UI feedback, not a fixed spinner) that
// someone trying the demo actually sees what a real save looks like.
const SAVE_DELAY_MS = 1800;
const CONNECT_DELAY_MS = 500; // believable "connecting…" beat; also lets App.tsx's client-keyed
// useEffect re-run and (re)bind onReading/onConnectionChange/etc. onto this instance before
// connect() actually fires them.

interface DemoStore {
  sensors: Sensor[];
  display: DisplayConfig;
  lego: LegoConfig;
  deviceName: string;
  version: number;
  hid: { connected: boolean; name: string };
  verboseDebug: boolean;
}

function hexToRgb(hex: string): [number, number, number] {
  const n = parseInt(hex.replace("#", ""), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

// A plausible captured reference for a taught colour, in whatever ref-vector shape the sensor
// type expects (see swatchFromRef in types.ts) — spreads an as7341's 10-band reference across
// its filter pairs the same rough way LegoConfigForm's seedManualRef does; every other colour
// sensor (tcs34725) just stores [r,g,b] directly.
function fakeColourRef(type: string, swatchHex: string): number[] {
  const [r, g, b] = hexToRgb(swatchHex);
  if (type === "as7341") {
    const ref = new Array(10).fill(0);
    ref[6] = ref[7] = r / 2; ref[3] = ref[4] = g / 2; ref[1] = ref[2] = b / 2;
    ref[8] = Math.max(r, g, b);
    return ref;
  }
  return [r, g, b];
}

function buildDemoDefaults(): DemoStore {
  let sensors: Sensor[] = [];
  const add = (d: Discovered, patch?: Partial<Sensor>): Sensor => {
    const s = { ...sensorFromDiscovered(d, sensors), ...patch };
    sensors = [...sensors, s];
    return s;
  };

  add(
    { bus: "i2c", addr: 0x29, guess: "tcs34725" },
    { name: "colour-sensor", transform: "col_rgb255", show: true },
  );
  const distance = add(
    { bus: "i2c", addr: 0x29, mux_addr: 0x70, channel: 0, guess: "vl53l1x" },
    { name: "distance-sensor", show: true },
  );
  add(
    { bus: "i2c", addr: 0x43, mux_addr: 0x70, channel: 1, guess: "m5_8angle" },
    { name: "knob-unit", led: 60, show: true },
  );
  const pad = { ...newGamepadSensor(sensors), name: "gamepad" };
  sensors = [...sensors, pad];

  const display: DisplayConfig = { ...defaultDisplay(), enabled: true, mode: "tiles" };

  // One LEGO quick-assign-shaped field so the LEGO tab already shows live packed data: the
  // distance sensor's mm reading -> the R channel of the RGBI word.
  const distMeta = sensorValueMeta(distance, 0);
  const { scale, offset } = distMeta
    ? scaleFromRange(distMeta.min, distMeta.max, 16, false)
    : { scale: 1, offset: 0 };
  const lego: LegoConfig = {
    ...defaultLego(),
    enabled: true,
    fields: [{ sensor_id: distance.id, value_index: 0, bits: 16, signed: false, scale, offset, target: LEGO_TARGET_RGBI }],
  };

  return {
    sensors,
    display,
    lego,
    deviceName: "Demo Board",
    version: 1,
    hid: { connected: false, name: "" },
    verboseDebug: false,
  };
}

// Extra devices discoverable via "Scan" — not already in the default sensor list, so demo mode
// stays scannable rather than purely static. Direct-bus (no mux_addr): SensorScanner only shows
// a mux-addressed device grouped under a matching "kind: mux" entry, which these don't have.
function demoScanResults(): Discovered[] {
  return [
    { bus: "i2c", addr: 0x39, guess: "as7341" },
    { bus: "i2c", addr: 0x48, guess: "m5_step16" },
  ];
}

export class MockBleClient extends BleClient {
  private _connected = false;
  private _name = "";
  private store: DemoStore = buildDemoDefaults();
  private tickTimer?: number;
  private hidTimer?: number;

  override get connected(): boolean {
    return this._connected;
  }
  override get deviceName(): string {
    return this._name;
  }

  override async connect(): Promise<void> {
    this.demoLog("demo mode: connecting…");
    await new Promise((r) => setTimeout(r, CONNECT_DELAY_MS));
    this._name = this.store.deviceName;
    this.demoLog(`demo mode: connected to ${this._name}`);
    this._connected = true;
    this.onConnectionChange?.(true);
  }

  // Not used by the demo entry point (no "remembered devices" concept), but kept so the
  // structural surface exactly matches BleClient — falls back to the same behaviour as connect().
  override async connectTo(): Promise<void> {
    await this.connect();
  }

  override disconnect(): void {
    this.stopTicking();
    if (this.hidTimer) { clearTimeout(this.hidTimer); this.hidTimer = undefined; }
    this._connected = false;
    this.demoLog("demo mode: disconnected");
    this.onConnectionChange?.(false);
  }

  private demoLog(msg: string): void {
    this.onLog?.(msg);
  }

  private startTicking(): void {
    if (this.tickTimer) return;
    this.tickTimer = window.setInterval(() => this.tick(), READING_TICK_MS);
  }
  private stopTicking(): void {
    if (this.tickTimer) { clearInterval(this.tickTimer); this.tickTimer = undefined; }
  }

  // One fake reading per enabled sensor per tick — a small bounded random-walk step per value,
  // clamped to that value's own catalogue range (sensorValueMeta), the same type-agnostic helper
  // the real UI already uses for every sensor type. No per-hardware-type branching needed.
  private walk = new Map<number, number[]>();
  private tick(): void {
    for (const s of this.store.sensors) {
      if (!s.enabled) continue;
      const names = sensorValues(s);
      const prev = this.walk.get(s.id) ?? [];
      const values = names.map((_, i) => {
        const meta = sensorValueMeta(s, i);
        const min = meta?.min ?? 0;
        const max = meta?.max ?? 100;
        const span = max - min || 1;
        const cur = prev[i] ?? min + Math.random() * span;
        const step = (Math.random() - 0.5) * span * 0.06;
        return Math.max(min, Math.min(max, cur + step));
      });
      this.walk.set(s.id, values);
      const reading: Reading = { sensor: s.id, ts: Date.now(), values, status: "ok" };
      this.onReading?.(reading);
    }
  }

  override request<T = any>(cmd: Record<string, unknown>): Promise<T> {
    const respond = (extra: Record<string, unknown>) =>
      Promise.resolve({ ok: true, id: cmd.id, ...extra } as unknown as T);

    switch (cmd.cmd) {
      case "scan":
        return Promise.resolve({ devices: demoScanResults() } as unknown as T);

      case "get_config":
        return Promise.resolve({
          version: this.store.version,
          sensors: this.store.sensors,
          display: this.store.display,
          lego: this.store.lego,
          device_name: this.store.deviceName,
          hid: this.store.hid,
          verbose_debug: this.store.verboseDebug,
          persist_ok: true,
          board: {
            name: "Demo Board", spi_cs_count: 5, has_uart: true, has_display: true, tft_controller: "st7789",
            tft_cs: 7, tft_dc: 39, tft_rst: 40, tft_bl: 45,
          },
        } as unknown as T);

      case "set_verbose_debug":
        this.store.verboseDebug = !!cmd.enabled;
        return respond({ verbose_debug: this.store.verboseDebug, version: ++this.store.version });

      case "set_lego_debug":
        this.store.lego.events = !!cmd.events;
        this.store.lego.debug = !!cmd.debug;
        return respond({ events: this.store.lego.events, debug: this.store.lego.debug, version: ++this.store.version });

      case "set_config": {
        // Mirrors the firmware's config_store_set_json(): a sensor object with no calib/colours
        // keeps whatever this store already has for that id, rather than losing it — App.tsx's
        // compactSensorsForSave() omits them whenever they're unchanged from the device (which
        // in demo mode means unchanged from *this* store), and this store is the only place that
        // "already has" comes from in mock mode. Without this, teaching a colour then hitting
        // Save in demo mode would silently wipe it back out on the very save meant to persist it.
        const incoming = (cmd.sensors as Sensor[] | undefined) ?? this.store.sensors;
        this.store.sensors = incoming.map((s) => {
          if (s.calib !== undefined && s.colours !== undefined) return s;
          const prev = this.store.sensors.find((p) => p.id === s.id);
          return {
            ...s,
            calib: s.calib ?? prev?.calib ?? [],
            colours: s.colours ?? prev?.colours,
          };
        });
        this.store.display = (cmd.display as DisplayConfig) ?? this.store.display;
        this.store.lego = (cmd.lego as LegoConfig) ?? this.store.lego;
        const version = ++this.store.version;
        return new Promise((resolve) =>
          window.setTimeout(() => resolve({ ok: true, id: cmd.id, version } as unknown as T), SAVE_DELAY_MS),
        );
      }

      case "set_device_name": {
        const name = String(cmd.name ?? "").slice(0, 19) || this.store.deviceName;
        this.store.deviceName = name;
        this._name = name;
        return respond({ device_name: name, version: ++this.store.version });
      }

      case "calibrate": {
        const s = this.store.sensors.find((x) => x.id === cmd.sensor_id);
        if (s) s.calib = (s.calib.length ? s.calib : [0, 0, 0, 0]).map((v) => v + (Math.random() - 0.5));
        return respond({ version: ++this.store.version, calib: s?.calib, colours: s?.colours });
      }

      case "learn_colour": {
        const s = this.store.sensors.find((x) => x.id === cmd.sensor_id);
        if (s) {
          const outId = Number(cmd.out_id);
          const nominal = SPIKE_COLOURS.find((c) => c.id === outId);
          const ref = fakeColourRef(s.type, nominal?.swatch ?? "#888888");
          const name = String(cmd.name ?? "");
          const next: ColourRef = { name, out_id: outId, learned: true, ref };
          s.colours = [...(s.colours ?? []).filter((c) => c.name !== name), next];
        }
        return respond({ version: ++this.store.version, colours: s?.colours });
      }

      case "reset_colour": {
        const s = this.store.sensors.find((x) => x.id === cmd.sensor_id);
        if (s) s.colours = (s.colours ?? []).filter((c) => c.name !== cmd.name);
        return respond({ version: ++this.store.version, colours: s?.colours });
      }

      case "reset_sensor": {
        const s = this.store.sensors.find((x) => x.id === cmd.sensor_id);
        if (s) { s.calib = []; s.colours = isColourSensor(s.type) ? [] : s.colours; }
        return respond({ version: ++this.store.version, calib: s?.calib, colours: s?.colours });
      }

      case "factory_reset":
        this.store = buildDemoDefaults();
        this.walk.clear();
        return respond({ version: this.store.version });

      case "hid_scan":
        if (this.hidTimer) clearTimeout(this.hidTimer);
        this.hidTimer = window.setTimeout(() => {
          this.store.hid = { connected: true, name: "Demo Controller" };
        }, 600);
        return respond({ connected: this.store.hid.connected });

      case "hid_forget":
        this.store.hid = { connected: false, name: "" };
        return respond({ connected: false });

      case "hid_virtual":
        return respond({ virtual: !!cmd.enabled });

      case "hid_set_state": {
        const pad = this.store.sensors.find((s) => s.type === "gamepad");
        const state = cmd as unknown as VirtualGamepadState;
        if (pad) this.walk.set(pad.id, [state.buttons, state.lx, state.ly, state.rx, state.ry, state.lt, state.rt, state.dpad]);
        return respond({ ok: true });
      }

      case "start":
      case "subscribe":
        this.startTicking();
        return respond({});

      case "stop":
      case "unsubscribe":
        this.stopTicking();
        return respond({});

      case "set_polling_cap":
        return respond({ version: ++this.store.version });

      default:
        return Promise.reject(new Error(`demo mode: unhandled command "${cmd.cmd}"`));
    }
  }
}
