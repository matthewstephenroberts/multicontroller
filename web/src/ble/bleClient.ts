// bleClient.ts — Web Bluetooth transport for the MultiController firmware.
//
// Framing matches the firmware: each message is [uint32 BE length][JSON payload].
// Requests carry an `id`; responses echo it. `reading` events have no id.
//
import { NUS_SERVICE, NUS_RX, NUS_TX } from "./uuids";
import type { Reading } from "../types";

// Each chunk is individually awaited (write-with-response — see writeChunks()'s own comment for
// why), so the total time a large set_config takes is roughly (payload / WRITE_CHUNK) sequential
// GATT round-trips, not just a raw radio-throughput limit. A real save observed in the field
// (9 sensors, 4 fully taught with 12 colours each — ~24KB) at the old 180B/chunk needed ~130
// round-trips; the OS/browser's own Bluetooth stack tore the connection down mid-transfer before
// the firmware ever saw a complete frame (NimBLE disconnect reason 531 = BLE_HS_HCI_ERR(0x13),
// "Remote User Terminated Connection" — the *central* gave up, not the firmware; no RX-overflow
// or protocol error was ever logged board-side). 244 is the widely-used safe default for
// pre-negotiation BLE 4.2+ links (ATT MTU 247 − 3-byte write header) — Web Bluetooth doesn't
// expose the actual negotiated MTU to JS to size this exactly, so this is a conservative
// almost-universally-safe value rather than the board's actual negotiated MTU (which this app
// has seen reach 512) — big enough to meaningfully cut round-trip count without risking a
// writeValue() rejection on a stack that negotiated less.
const WRITE_CHUNK = 244;

type Pending = {
  resolve: (v: any) => void;
  reject: (e: Error) => void;
  timer: number;
};

export class BleClient {
  private device?: BluetoothDevice;
  private rx?: BluetoothRemoteGATTCharacteristic; // host -> device
  private tx?: BluetoothRemoteGATTCharacteristic; // device -> host
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private rxBuf = new Uint8Array(0);
  private writeChain: Promise<void> = Promise.resolve();

  onReading?: (r: Reading) => void;
  onMatrix?: (pixels: string[]) => void; // 3×3 Light Matrix pixels from the hub ("#rrggbb"×9)
  onHid?: (connected: boolean, name: string) => void; // BLE-HID controller connect/disconnect
  onConnectionChange?: (connected: boolean) => void;
  onLog?: (msg: string) => void;

  get connected(): boolean {
    return !!this.device?.gatt?.connected;
  }
  get deviceName(): string {
    return this.device?.name ?? "";
  }

  static get supported(): boolean {
    return typeof navigator !== "undefined" && !!navigator.bluetooth;
  }

  // Boards this origin has already been granted access to (Chrome's permission-backed
  // getDevices()) — reconnecting to one of these skips the browser chooser entirely, so the
  // app can present its own picker. First-time access to a NEW board always needs the native
  // requestDevice() chooser: Web Bluetooth forbids sites from enumerating nearby devices, so
  // no custom UI can replace that first grant. Returns [] where getDevices is unavailable.
  static async rememberedDevices(): Promise<BluetoothDevice[]> {
    const bt = BleClient.supported ? navigator.bluetooth as Bluetooth & { getDevices?: () => Promise<BluetoothDevice[]> } : undefined;
    if (!bt || typeof bt.getDevices !== "function") return [];
    try {
      return await bt.getDevices();
    } catch {
      return [];
    }
  }

  // Forget a previously-granted board (removes it from the remembered list). Not supported
  // in every browser — silently no-ops there.
  static async forgetDevice(dev: BluetoothDevice): Promise<void> {
    try {
      await (dev as BluetoothDevice & { forget?: () => Promise<void> }).forget?.();
    } catch { /* unsupported — the entry just stays remembered */ }
  }

  // Connect via the browser's native chooser (required the first time for any new board).
  async connect(): Promise<void> {
    // Verbose step-by-step logging (surfaces in the app's own Activity log, no DevTools/
    // terminal needed) — connect() spans several awaited steps any of which can silently hang
    // or reject in a packaged/Electron context in ways that are otherwise invisible.
    this.log(`navigator.bluetooth: ${typeof navigator !== "undefined" && navigator.bluetooth ? "available" : "MISSING"}`);
    if (!BleClient.supported) {
      throw new Error("Web Bluetooth not available — use Chrome or Edge on desktop.");
    }
    this.log("requesting device (filters: NUS service)…");
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }],
      optionalServices: [NUS_SERVICE],
    });
    await this.connectTo(device);
  }

  // Connect to an already-granted device (from rememberedDevices()) — no browser chooser.
  async connectTo(device: BluetoothDevice): Promise<void> {
    this.device = device;
    this.log(`device selected: ${device.name ?? "(unnamed)"}`);
    device.addEventListener("gattserverdisconnected", () => this.handleDisconnect());

    this.log("connecting GATT server…");
    const server = await device.gatt!.connect();
    this.log("getting primary service…");
    const svc = await server.getPrimaryService(NUS_SERVICE);
    this.log("getting characteristics…");
    this.rx = await svc.getCharacteristic(NUS_RX);
    this.tx = await svc.getCharacteristic(NUS_TX);

    this.log("starting notifications…");
    await this.tx.startNotifications();
    this.tx.addEventListener("characteristicvaluechanged", (e) =>
      this.onNotify((e.target as BluetoothRemoteGATTCharacteristic).value!),
    );

    this.log(`connected to ${this.deviceName}`);
    this.onConnectionChange?.(true);
  }

  disconnect(): void {
    this.device?.gatt?.disconnect();
  }

  private handleDisconnect(): void {
    this.log("disconnected");
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(new Error("disconnected"));
    }
    this.pending.clear();
    this.rxBuf = new Uint8Array(0);
    this.writeChain = Promise.resolve();
    this.onConnectionChange?.(false);
  }

  // Send a command and resolve with its response (matched by id). Responses arrive as BLE
  // *notifications*, which have no delivery guarantee at the protocol level (unlike a write
  // with response) — an occasional one can simply be lost over the air with neither side aware
  // of it, which otherwise silently breaks whatever the command was supposed to do (a dropped
  // "subscribe" ack means live data never starts flowing even though the device applied it
  // fine). So a timeout here resends the identical request (same id) a couple of times before
  // finally giving up — commands are all idempotent to retry (start/subscribe/teach/etc. all
  // behave the same whether applied once or twice), and a late original response arriving after
  // a retry is harmless: dispatch() only acts on the first one, since resolving deletes the id
  // from `pending`.
  request<T = any>(cmd: Record<string, unknown>, timeoutMs = 8000, maxAttempts = 2): Promise<T> {
    if (!this.rx) return Promise.reject(new Error("not connected"));
    const id = this.nextId++;
    const json = JSON.stringify({ ...cmd, id });
    const frame = this.frame(json);

    return new Promise<T>((resolve, reject) => {
      let attempt = 0;
      const send = () => {
        attempt++;
        const timer = window.setTimeout(() => {
          if (attempt < maxAttempts && this.connected) {
            this.log(`no response for "${cmd.cmd}" — retrying (${attempt}/${maxAttempts - 1})…`);
            send();
            return;
          }
          this.pending.delete(id);
          reject(new Error(`timeout waiting for "${cmd.cmd}"`));
        }, timeoutMs);
        this.pending.set(id, { resolve, reject, timer });
        this.writeFrame(frame).catch((e) => {
          clearTimeout(timer);
          this.pending.delete(id);
          reject(e);
        });
      };
      send();
    });
  }

  // ---- framing ----
  private frame(json: string): Uint8Array {
    const body = new TextEncoder().encode(json);
    const out = new Uint8Array(4 + body.length);
    new DataView(out.buffer).setUint32(0, body.length, false); // big-endian
    out.set(body, 4);
    return out;
  }

  // Serialises every write through one queue: Web Bluetooth GATT operations on a characteristic
  // aren't reentrant — issuing a new writeValue() before the previous one resolves throws (or on
  // some platforms silently wedges the connection). request() has no idea how many other
  // request()s are in flight (e.g. the virtual controller can fire several a second while
  // dragging a stick, concurrently with a save/calibrate/etc.), so this queue — not the caller —
  // is what guarantees writes never overlap. A failed write only rejects its own caller; the
  // chain itself always continues so one dropped command can't wedge every write after it.
  private writeFrame(frame: Uint8Array): Promise<void> {
    const run = () => this.writeChunks(frame);
    const result = this.writeChain.then(run, run);
    this.writeChain = result.then(
      () => undefined,
      () => undefined,
    );
    return result;
  }

  private async writeChunks(frame: Uint8Array): Promise<void> {
    for (let off = 0; off < frame.length; off += WRITE_CHUNK) {
      // Write WITH response (awaits an ACK per chunk) so multi-chunk configs aren't dropped
      // by the controller — write-without-response has no flow control and loses packets
      // under load, which made saves intermittently fail. slice() gives an ArrayBuffer copy.
      await this.rx!.writeValue(frame.slice(off, off + WRITE_CHUNK));
    }
  }

  private onNotify(value: DataView): void {
    // Append incoming bytes into a fresh contiguous buffer (offset 0).
    const incoming = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const merged = new Uint8Array(this.rxBuf.length + incoming.length);
    merged.set(this.rxBuf, 0);
    merged.set(incoming, this.rxBuf.length);

    // Extract every complete [uint32 len][payload] frame.
    const view = new DataView(merged.buffer);
    let off = 0;
    for (;;) {
      if (merged.length - off < 4) break;
      const len = view.getUint32(off, false);
      if (merged.length - off < 4 + len) break;
      const json = new TextDecoder().decode(merged.subarray(off + 4, off + 4 + len));
      off += 4 + len;
      this.dispatch(json);
    }
    this.rxBuf = merged.slice(off); // keep the partial remainder, offset reset to 0
  }

  private dispatch(json: string): void {
    let msg: any;
    try {
      msg = JSON.parse(json);
    } catch {
      this.log(`bad json: ${json.slice(0, 64)}`);
      return;
    }
    if (msg.type === "reading") {
      this.onReading?.(msg as Reading);
      return;
    }
    if (msg.type === "lego_matrix") {
      this.onMatrix?.(msg.pixels as string[]);
      return;
    }
    if (msg.type === "hid") {
      this.onHid?.(!!msg.connected, (msg.name as string) ?? "");
      return;
    }
    if (typeof msg.id === "number" && this.pending.has(msg.id)) {
      const p = this.pending.get(msg.id)!;
      clearTimeout(p.timer);
      this.pending.delete(msg.id);
      if (msg.ok === false) p.reject(new Error(msg.error ?? "device error"));
      else p.resolve(msg);
    }
  }

  private log(msg: string): void {
    this.onLog?.(msg);
  }
}
