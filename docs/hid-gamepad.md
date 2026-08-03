# Bluetooth game controller → LEGO hub

The board can host a **Bluetooth LE game controller** (Xbox Series X|S) as a BLE-HID device,
expose it as a `gamepad` virtual sensor, and — through the [LEGO colour-sensor emitter](lego-emit.md)
— send its buttons/sticks to a SPIKE Prime / Pybricks hub. A hub program reads the state with
`color_sensor.rgbi()` and the generated decoder, so a controller can drive LEGO motors/logic.

```
Xbox controller ──BLE-HID──▶ ESP32-S3 (NimBLE central) ──▶ gamepad sensor
   ──▶ scheduler/transform ──▶ lego_emit bit-packer ──▶ UART ──▶ LEGO hub
```

## Hard constraint: BLE only

The ESP32-S3 has **Bluetooth LE only — no Bluetooth Classic**. So:

- ✅ Works: BLE-HID controllers — **Xbox Series X|S** (Bluetooth mode), 8BitDo in BLE modes,
  generic "BLE gamepad" modules.
- ❌ Won't work: Classic-only pads — **PS4 DualShock, PS5 DualSense**, older Wii, etc. The
  radio physically can't talk to them.

The firmware runs NimBLE in **dual role**: peripheral (the web-app link) *and* central (the
controller), as two concurrent connections (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`).

## Pairing

1. Add a sensor of type **`gamepad`** (any `poll_ms`, e.g. 50 ms; bus/addr are ignored) — or click
   **+ Add gamepad sensor** in the Game controller card, which does this for you. It has to exist
   in the sensor list before its buttons/sticks can be picked as a LEGO field source or shown on
   the dashboard, whether you're using a real pad or the virtual controller.
2. In the web **Game controller (BLE-HID)** card, click **Pair / Scan**.
3. Put the Xbox controller in pairing mode — hold the **Pair** button (top edge) until the
   Xbox light flashes rapidly.
4. The board scans for the HID service (`0x1812`), connects, bonds with **LE Secure
   Connections** (Just-Works), and subscribes to the input reports. The card shows the
   controller name and live button/stick state.

The bond is **persisted to NVS**, so after a reboot the board auto-reconnects when the
controller is on. **Forget** erases the bond (re-pair to reconnect).

## Virtual controller (no pad needed)

The **Game controller** card has a **virtual controller** checkbox: enable it and an on-screen
Xbox layout appears (sticks, triggers, dpad, buttons) that drives the `gamepad` sensor exactly
like a real pad — same `buttons`/axis values flow through the scheduler, LEGO emitter, and
dashboard. Useful for testing a LEGO field mapping or hub program without a physical controller.

While enabled, the board ignores real HID reports for the `gamepad` sensor (a real pad can stay
connected underneath — its reports keep arriving, they're just not what the sensor reads).
Un-check the box to go back to a real pad's live state. Dragging a stick/trigger sends state at
up to ~20 updates/sec; button presses and releases are sent immediately.

## The `gamepad` sensor values

| Index | Name | Range | Notes |
|------:|------|-------|-------|
| 0 | `buttons` | 0–65535 | bitmask, see below |
| 1–4 | `lx ly rx ry` | −32768..32767 | sticks, centred |
| 5–6 | `lt rt` | 0–1023 | triggers |
| 7 | `dpad` | 0–8 | 8-way hat (0 = released) |

**Button bitmask** (`buttons`): bit 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 View, 7 Menu, 8 LS,
9 RS, 10 Xbox, 11 Share, 12 D-up, 13 D-down, 14 D-left, 15 D-right. (Mirrors `HID_BTN_*` in
`firmware/components/hid_host/hid_host.h`; the report layout is the Xbox Series BLE format —
adjust `parse_report()` there if a controller's bits differ, using the web live-viz to confirm.)

## Sending it to the hub

In the LEGO emitter card, add fields for the gamepad sensor — e.g. a **16-bit `buttons`**
field (→ R channel), and optionally axes (8-bit each, with min/max −32768..32767). Save. The
**generated decoder** reconstructs them on the hub; test a button bit, e.g. on SPIKE:

```python
r, g, b, i = color_sensor.rgbi(port.A)
buttons = r            # the 16-bit buttons field landed in channel R
A = bool(buttons & 1)  # bit 0 = A
if A:
    motor.run(port.B, 500)
```

## BLE protocol

- `{cmd:"hid_scan"}` → start scanning/pairing. `{cmd:"hid_forget"}` → erase the bond.
- Event `{type:"hid", connected:bool, name:str}` is streamed on connect/disconnect.
- `{cmd:"hid_virtual", enabled}` → toggle the virtual controller. `{cmd:"hid_set_state", buttons,
  lx, ly, rx, ry, lt, rt, dpad}` → push a virtual state snapshot (only applied while enabled).

See [ble-protocol.md](ble-protocol.md).

## Notes / limits

- One controller at a time (the central holds a single HID connection).
- Pairing requires the controller in pairing mode on first bond; afterwards it auto-reconnects.
- If pairing fails repeatedly, **Forget** then re-pair (a stale bond on either side blocks
  encryption).
