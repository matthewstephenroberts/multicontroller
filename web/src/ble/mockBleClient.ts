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

  // Every type in NAMED_TYPES gets an instance, so demo mode can exercise each one's own editor
  // controls, dashboard card and LEGO-field behaviour without the hardware — previously only
  // tcs34725/vl53l1x/m5_8angle/gamepad existed, and the other 17 types had no way to be tried at
  // all (the ones the scanner can't identify by address — SPI, DA-pin, module boards — couldn't
  // even be reached through demo Scan). Addresses/buses mirror what bus_scan.c actually reports
  // for each part, so what demo mode shows lines up with a real scan.
  //
  // `show` (the onboard display) stays limited to the curated four: every sensor still streams
  // and appears in the Sensors tab and the LEGO field pickers, but the little TFT is a 3x3-ish
  // space and showing 20 sensors on it isn't a useful default. Four types are deliberately left
  // out of this list and offered through Scan instead, so demo Scan still finds something new.
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

  // --- environment / motion / power (I2C, all address-identifiable) ---
  add({ bus: "i2c", addr: 0x76, guess: "bmp280" }, { name: "pressure" });
  // 0x76/0x77 guess as bmp280 either way — the humidity element is what makes it a bme280, and
  // the user picks the right one when adding, exactly as the scanner's own comment says.
  add({ bus: "i2c", addr: 0x77, guess: "bmp280" }, { name: "weather", type: "bme280" });
  add({ bus: "i2c", addr: 0x6b, guess: "qmi8658" }, { name: "imu", transform: "imu_orient", show: true });
  // Onboard AtomS3R stack — no address/mux, the driver ignores that config entirely.
  add({ bus: "i2c", guess: "bmi270_bmm150" }, { name: "onboard-imu", transform: "imu_orient9" });
  add({ bus: "i2c", addr: 0x40, guess: "ina226" }, { name: "battery" });
  add({ bus: "i2c", addr: 0x65, guess: "vk36n16" }, { name: "touch-keys" });

  // --- distance module boards: no ID register, so the scanner can't guess these types ---
  add({ bus: "i2c", addr: 0x52, guess: "unknown" }, { name: "tof-module", type: "tof10120" });
  add({ bus: "i2c", addr: 0x08, guess: "unknown" }, { name: "tof-generic", type: "tofi2c" });

  // --- board DA pins: no bus device at all, `port` is the GPIO (see BOARD_DA_PINS) ---
  add({ bus: "i2c", guess: "unknown" }, { name: "button-pin", type: "gpio", port: 18 });
  add({ bus: "i2c", guess: "unknown" }, { name: "analog-pin", type: "adc", port: 17, transform: "adc_volts" });

  // --- SPI: no addressing scheme, so the scanner reports CS lines as "unknown" (BUS_DRIVER_TYPES)
  // and the type is always picked by hand. qre1113/tssp_ir use channel_mask for the grouped
  // multi-channel shape (a line-follower bar / IR ring read as one sensor).
  add({ bus: "spi", cs_index: 0, guess: "unknown" }, { name: "adc-8ch", type: "mcp3208", port: 0 });
  add(
    { bus: "spi", cs_index: 0, guess: "unknown" },
    { name: "line-array", type: "qre1113", channel_mask: 0b00001111, transform: "line_reflect" },
  );
  add(
    { bus: "spi", cs_index: 0, guess: "unknown" },
    { name: "ir-ring", type: "tssp_ir", channel_mask: 0b00000111, transform: "ir_ball" },
  );

  // --- generic: the custom register-recipe escape hatch, with a recipe that actually decodes ---
  add(
    { bus: "i2c", addr: 0x5a, guess: "unknown" },
    {
      name: "custom-device",
      type: "generic",
      recipe: { reg: 0x00, length: 2, byte_order: "be", signed: false, scale: 0.1, offset: 0, value_names: ["level"] },
    },
  );

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
    // The mux itself must be reported for anything behind it to be visible: SensorScanner only
    // renders a mux-attached device inside the group for its `kind: "mux"` entry, so a device
    // carrying a mux_addr with no matching mux entry is dropped from BOTH lists and shows up
    // nowhere at all. Real firmware reports detected muxes the same way (bus_scan.c step 1b),
    // and the demo's default sensors already sit on this 0x70 mux.
    { bus: "i2c", kind: "mux", addr: 0x70, channels: 8, guess: "tca9548a" },
    // vl53l0x shares 0x29 with tcs34725/vl53l1x on real hardware — the scanner tells them apart
    // by chip ID. Behind the mux, like the vl53l1x already in the default list.
    { bus: "i2c", addr: 0x29, mux_addr: 0x70, channel: 2, guess: "vl53l0x" },
    // Wired SPI chip-selects. Real firmware reports every CS line it has (bus_scan.c step 4) and
    // always as "unknown" — SPI has no addressing or ID scheme to probe, so the type is picked by
    // hand from BUS_DRIVER_TYPES.spi (mcp3208 / qre1113 / tssp_ir / generic). Without these, that
    // whole add-flow — the one used for every SPI sensor — had nothing to demonstrate it.
    { bus: "spi", cs_index: 0, guess: "unknown" },
    { bus: "spi", cs_index: 1, guess: "unknown" },
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
