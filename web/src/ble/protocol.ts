// protocol.ts — typed command wrappers over BleClient.request().
import { BleClient } from "./bleClient";
import type { ColourRef, Discovered, DisplayConfig, LegoConfig, Sensor, VirtualGamepadState } from "../types";

// Calibrate/Teach/Reset all hand back the one sensor's freshly-captured calib/colours directly
// in their response now, instead of the caller needing a full get_config refetch just to see
// what changed — that refetch (not the flash write, which is already async) was the actual
// source of the per-action delay these commands used to have.
interface CalibColoursResponse {
  version: number;
  calib?: number[];
  colours?: ColourRef[];
}

export const api = {
  // maxAttempts=1: a real bus scan can legitimately take close to the full 20s timeout, so a
  // "timeout" here is more likely genuine slowness than a dropped response — retrying would
  // just re-trigger a second full scan on top of one that may still be running.
  scan: (c: BleClient) =>
    c.request<{ devices: Discovered[] }>({ cmd: "scan" }, 20000, 1).then((r) => r.devices),

  // get_config's response is the *whole* store (all sensors, incl. any taught colour palettes)
  // — for a multi-sensor config that's easily 10+ KB, meaning 20+ BLE notify chunks at typical
  // MTU. The firmware paces/retries those chunks against its own mbuf-pool congestion (see
  // ble_svc.c's send_framed()), which is a legitimate, not-transient reason a large response can
  // take several seconds under real contention (concurrent reading-event streaming, LEGO UART
  // traffic, etc.) — the default 8s timeout was sized for ordinary small command responses and
  // was timing out (then retrying, doubling the wait) on nothing more than an honestly slow but
  // still in-flight transfer. 20s gives it real room before giving up.
  getConfig: (c: BleClient) =>
    c.request<{
      version: number; sensors: Sensor[]; display: DisplayConfig; lego: LegoConfig;
      device_name?: string; polling_cap_us?: number;
      // Snapshot of the BLE-HID gamepad status — the board auto-reconnects a paired controller
      // on its own, so it may already be connected before this client attached (push events
      // alone would leave the Gamepad card stuck on "not connected").
      hid?: { connected: boolean; name: string };
      verbose_debug?: boolean;
      // Whether the last background flash write actually landed — a save can apply to the
      // board's RAM and return ok:true before its async NVS commit runs, so this can only be
      // known on a *later* get_config, not the set_config response itself.
      persist_ok?: boolean;
      // Board hardware capabilities (BOARD_NAME/BOARD_SPI_CS_COUNT/BOARD_HAS_DISPLAY/
      // BOARD_TFT_CONTROLLER/BOARD_TFT_CS_DC_RST_BL_GPIO from board_config.h) — lets the UI hide/
      // limit controls the current board's firmware build can't act on (e.g. no SPI CS lines
      // wired at all on boards like AtomS3 Lite/AtomS3R), show which physical board this
      // firmware was built for, and reset the Display tab to this board's actual wired
      // controller/pins on demand — available immediately at connect time, not just after
      // running a scan, so a stale/wrong saved display config is always recoverable.
      board?: {
        name: string; spi_cs_count: number; has_uart?: boolean; has_display: boolean; tft_controller: string;
        tft_cs: number; tft_dc: number; tft_rst: number; tft_bl: number;
      };
    }>({
      cmd: "get_config",
    }, 20000),

  // Verbose sensor debug logging on the device's serial console (distance-sensor range
  // diagnostics, BLE-HID first-report hex dumps) — off by default, persisted on the board.
  setVerboseDebug: (c: BleClient, enabled: boolean) =>
    c.request<{ verbose_debug: boolean; version: number }>({ cmd: "set_verbose_debug", enabled }),

  // LEGO emitter serial-log verbosity — same instant-apply shape as setVerboseDebug (not part
  // of setConfig's Save flow), so all three device debug toggles behave identically.
  setLegoDebug: (c: BleClient, events: boolean, debug: boolean) =>
    c.request<{ events: boolean; debug: boolean; version: number }>({ cmd: "set_lego_debug", events, debug }),

  // Same slow-payload reasoning as getConfig above, in the other direction: a fully-populated
  // multi-sensor config (calibration + taught colour palettes for every sensor) can be tens of
  // KB, chunked over BLE at ~180B/write-with-response plus the firmware's own cJSON_Parse of
  // that whole payload — comfortably past the old default 8s on a large config, which is what
  // was surfacing as "save errors/timeout" as more sensors got added. 20s matches getConfig's
  // budget for the same size of transfer. maxAttempts=1 (not the default retry-twice): unlike
  // small idempotent commands, a timed-out attempt here may simply still be mid-flight rather
  // than lost — retrying would queue a second full multi-KB resend behind the first (writes are
  // serialised, so it can't corrupt anything, but it doubles the transfer + a second firmware
  // parse/persist/version-bump for one Save click) instead of just giving the first attempt more
  // room to land.
  setConfig: (c: BleClient, sensors: Sensor[], display: DisplayConfig, lego: LegoConfig) =>
    c.request<{ version: number }>({ cmd: "set_config", sensors, display, lego }, 20000, 1),

  // Rename the board's BLE device name (1-19 characters). Takes effect on the GAP device-name
  // characteristic immediately, and in the advertised name (what shows up in a scan) the next
  // time this connection ends and the board starts advertising again.
  setDeviceName: (c: BleClient, name: string) =>
    c.request<{ device_name: string; version: number }>({ cmd: "set_device_name", name }),

  // Capture calibration for a sensor from its latest reading (per its transform mode). `point`
  // selects which point a multi-point mode captures (e.g. "line_reflect"'s "white"/"black").
  calibrate: (c: BleClient, sensorId: number, point?: string) =>
    c.request<CalibColoursResponse>({ cmd: "calibrate", sensor_id: sensorId, ...(point ? { point } : {}) }),

  // Teach a colour: capture the current reading as `name`→`outId` in the sensor's palette.
  learnColour: (c: BleClient, sensorId: number, name: string, outId: number) =>
    c.request<CalibColoursResponse>({ cmd: "learn_colour", sensor_id: sensorId, name, out_id: outId }),
  // Reset a colour to default (remove its learned/custom palette entry).
  resetColour: (c: BleClient, sensorId: number, name: string) =>
    c.request<CalibColoursResponse>({ cmd: "reset_colour", sensor_id: sensorId, name }),
  // Per-sensor factory reset: wipe one sensor's calibration + taught colours on the device
  // (its type/bus/pins config stays). Much lighter than a full set_config round-trip.
  resetSensor: (c: BleClient, sensorId: number) =>
    c.request<CalibColoursResponse>({ cmd: "reset_sensor", sensor_id: sensorId }),
  // Erase all config (sensors/display/lego/colours) back to board defaults.
  factoryReset: (c: BleClient) => c.request<{ version: number }>({ cmd: "factory_reset" }),

  // BLE-HID game controller (Xbox Series): pair/scan, or erase the bond.
  hidScan: (c: BleClient) => c.request<{ connected: boolean }>({ cmd: "hid_scan" }),
  hidForget: (c: BleClient) => c.request<{ connected: boolean }>({ cmd: "hid_forget" }),

  // Virtual on-screen controller: no physical pad needed. Enable, then push state snapshots —
  // the `gamepad` sensor reads the virtual state instead of a real pad while enabled.
  hidSetVirtual: (c: BleClient, enabled: boolean) =>
    c.request<{ virtual: boolean }>({ cmd: "hid_virtual", enabled }),
  hidSetState: (c: BleClient, state: VirtualGamepadState) =>
    c.request<{ ok: boolean }>({ cmd: "hid_set_state", ...state }),

  start: (c: BleClient) => c.request({ cmd: "start" }),
  stop: (c: BleClient) => c.request({ cmd: "stop" }),
  subscribe: (c: BleClient) => c.request({ cmd: "subscribe" }),
  unsubscribe: (c: BleClient) => c.request({ cmd: "unsubscribe" }),
  setPollingCap: (c: BleClient, capHz: number) =>
    c.request<{ version: number }>({ cmd: "set_polling_cap", cap_hz: capHz }),
};
