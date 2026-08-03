# MultiController BLE protocol

Single source of truth for the wire contract between the firmware and the web app.

## Transport

A custom **Nordic-UART-style** GATT service (128-bit UUIDs):

| Role     | UUID                                   | Properties              |
|----------|----------------------------------------|-------------------------|
| Service  | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | —                       |
| **RX**   | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | Write, Write-No-Response (host → device) |
| **TX**   | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Notify (device → host)  |

- Device advertises name **`MultiController`** plus the service UUID.
- The client requests an ATT **MTU of 512**.

### Framing

GATT writes/notifications are bounded by the negotiated MTU, but messages (configs, scan
results) can be larger. Every logical message is therefore framed:

```
+--------------------+-------------------------------+
| len  (4 bytes, BE) | JSON payload  (len bytes)     |
+--------------------+-------------------------------+
```

The sender splits `frame = len32_be(json) + json` into MTU-3 sized chunks across successive
writes/notifications; the receiver buffers until it has `len` bytes, then parses one JSON
message. `len` must not exceed 64 KiB.

## Messages

All messages are JSON objects. Requests carry a client-chosen integer `id`; the matching
response echoes it. Events have no `id` and may arrive at any time after `subscribe`.

### Commands (host → device)

| `cmd`        | extra fields            | response                                              |
|--------------|-------------------------|------------------------------------------------------|
| `scan`       | —                       | `{id, ok, devices:[Discovered]}`                     |
| `get_config` | —                       | `{id, ok, version, display:Display, sensors:[Sensor], lego:Lego, device_name:string}` |
| `set_config` | `sensors:[Sensor], display:Display, lego:Lego` | `{id, ok, version}` — validates, applies in memory and rebuilds schedule immediately, re-applies the LEGO emitter. `display` and `lego` are optional (unset fields keep current values). The actual NVS flash write happens asynchronously on a background task (see config_store.c) — `version` already reflects the new config, but there's a brief window after the response where a power loss would lose that save, in exchange for not blocking the BLE link for the ~1s+ a flash commit can take. |
| `start`      | —                       | `{id, ok}` — start polling                            |
| `stop`       | —                       | `{id, ok}` — stop polling                             |
| `subscribe`  | —                       | `{id, ok}`, then a stream of `reading` events, throttled to at most 50Hz per sensor regardless of its `poll_ms` — a fast internal poll rate (e.g. 5-10ms floors on `mcp3208`/`qre1113`/`gpio`/`adc`) is useful for the sensor itself, but streaming every single poll over BLE would exceed what one connection can notify. |
| `unsubscribe`| —                       | `{id, ok}`                                            |
| `calibrate`  | `sensor_id, point?`     | `{id, ok, version}` — captures calibration for that sensor from its latest reading (per its transform mode), persists to NVS. `point` is only used by multi-point modes (`line_reflect`'s `"white"`/`"black"`); omit it for one-shot modes. |
| `learn_colour` | `sensor_id, name, out_id` | `{id, ok, version}` — teach a colour: capture the current reading as `name`→`out_id` in the sensor's palette. See [colour-calibration.md](colour-calibration.md). |
| `reset_colour` | `sensor_id, name`     | `{id, ok, version}` — remove a learned/custom colour (reverts to default). |
| `factory_reset` | —                      | `{id, ok, version}` — erase all NVS config (sensors/display/lego/colours) and restore board defaults, including the device name. |
| `set_device_name` | `name` (1-19 chars) | `{id, ok, device_name, version}` — rename the board's BLE device. Applied to the GAP device-name characteristic immediately; the *advertised* name (what shows up in a scan) updates the next time advertising restarts — i.e. after this connection ends. `{id, ok:false, error}` for an empty name or one over 19 characters. |
| `hid_scan`   | —                       | `{id, ok, connected}` — scan/pair a BLE-HID game controller (see [hid-gamepad.md](hid-gamepad.md)). |
| `hid_forget` | —                       | `{id, ok, connected}` — erase the controller bond and disconnect. |
| `hid_virtual` | `enabled`              | `{id, ok, virtual}` — enable/disable the on-screen virtual controller (see [hid-gamepad.md](hid-gamepad.md)). While enabled, the `gamepad` sensor reads the virtual state instead of a real pad. |
| `hid_set_state` | `buttons, lx, ly, rx, ry, lt, rt, dpad` | `{id, ok}` — push a virtual controller state snapshot (only takes effect while `hid_virtual` is enabled). Same field layout as the `gamepad` sensor's reading. |

On error, the response is `{id, ok:false, error:"<message>"}`.

### Events (device → host)

```jsonc
{ "type": "reading",
  "sensor": 3,              // sensor id
  "ts": 1530000,           // device ms since boot
  "values": [22.5, 41.2],  // parallel to the sensor's value_names
  "status": "ok"           // "ok" | "error" | "timeout"
}
```

When the LEGO emitter runs the **3×3 Light Matrix** profile and the hub writes pixels, the
device also streams (to subscribers):

```jsonc
{ "type": "lego_matrix",
  "pixels": ["#FF0000", "#00FF00", "#0000FF", "#000000", … ]  // 9 cells, row-major "#rrggbb"
}
```

A BLE-HID game controller connecting/disconnecting (see [hid-gamepad.md](hid-gamepad.md)):

```jsonc
{ "type": "hid", "connected": true, "name": "Xbox Wireless Controller" }
```

## Object shapes

### Sensor

```jsonc
{
  "id": 1,                  // unique within the config
  "name": "Cabin temp",
  "type": "generic",        // "generic" (uses recipe) or a named driver, e.g. "bme280"
  "bus": "i2c",             // "i2c" | "spi" | "uart"

  // addressing — only the fields relevant to `bus` are used:
  "addr": 118,              // i2c: 7-bit address (0x76)
  "mux_addr": 112,          // i2c: TCA9548A address (0x70), or 0 / omitted for direct
  "mux_channel": 2,         // i2c: 0..7 behind the mux (ignored if no mux)
  "cs_index": 0,            // spi: 0..4 -> which configured CS GPIO
  "port": 1,                // uart: UART port number

  // generic register recipe (ignored when `type` is a named driver):
  "recipe": {
    "reg": 250,             // start register / command
    "length": 6,            // bytes to read
    "byte_order": "be",     // "be" | "le"
    "signed": false,
    "scale": 0.01,          // value = raw*scale + offset
    "offset": 0,
    "value_names": ["temp"] // one entry per decoded value
  },

  // derived-value transform applied after the raw read (see docs/lego-emit.md):
  "transform": "raw",       // "raw" | "imu_orient" | "imu_tilt" | "col_rgb255" | "col_hue"
                            //   | "col_lego" | "col_full" | "as_lego" | "as_full" | "as_dist"
                            //   | "dist_mm" | "dist_cm"   (col_lego/as_lego emit official
                            //   SPIKE colour ids; *_full = colour+reflect+RGB for passthrough)
  "calib": [],              // per-sensor calibration scalars, captured via the `calibrate` cmd
  "colours": [              // colour sensors: learnable palette (omitted when empty)
    { "name": "red", "out_id": 9, "learned": true, "ref": [/* 3 or 10 floats */] }
  ],                        // calib + colours are editable via set_config for manual fine-tuning
  "colour_smooth": 0,       // colour sensors: EMA noise filter on raw channels, 0 (off) - 0.95
  "colour_debounce": 0,     // colour sensors: consecutive matches before reported id changes; 0 = off

  // distance sensors (vl53l1x/tof10120/tofi2c): configurable measuring range. The reading is
  // clamped to [dist_min_mm, dist_max_mm], and the web UI's LEGO field editor reads this range
  // back to auto-derive its scale/offset — no manual scale tuning needed for a distance field.
  "dist_mode": 0,           // vl53l1x only: 0 = short range (≤1.3m, faster), 1 = long (≤4m,
                            //   slower — needs a longer integration time, so poll_ms is floored
                            //   higher: 60ms short / 140ms long, enforced in config_store)
  "dist_min_mm": 0,         // 0 = use the mode's native minimum
  "dist_max_mm": 0,         // 0 = use the mode's native maximum

  "poll_ms": 1000,          // poll interval — clamped up on save to this sensor type's
                            //   realistic floor (hardware integration time + I2C overhead;
                            //   see sensor_poll_floor_ms() in firmware/components/sensor/
                            //   sensor.c): 10ms qmi8658/gpio/adc/gamepad, 15ms tcs34725,
                            //   20ms bme280/bmp280, 40ms tof10120, 50ms generic/tofi2c,
                            //   60/140ms vl53l1x short/long, 100ms as7341
  "enabled": true,
  "simulate": false,        // generate plausible random data instead of reading the real bus
                            // (no mux select, no I2C/SPI/UART transaction) — same poll_ms
                            // schedule, so dashboard/display/LEGO emit all work without hardware
  "show": false,            // show this sensor on the display
  "page": 0                 // display page/group (paged mode)
}
```

When a `transform` other than `raw` is set, the sensor's reported values change to the mode's
derived outputs (e.g. `imu_orient` → roll/pitch/yaw; `col_hue` → hue/sat/val). The dashboard,
display, and LEGO emitter all see the transformed values. `calib` is type-specific (colour
white reference, IMU gyro bias, distance zero offset) and is captured on the device by the
`calibrate` command, not edited by hand.

### Display

```jsonc
{
  "enabled": true,
  "controller": "st7789",   // "st7789" | "ili9341" (SPI) | "ssd1306" (I2C)
  "bus": "spi",             // "spi" | "i2c"
  "cs": 7, "dc": 39, "rst": 40, "bl": 45,   // SPI pins (-1 = none)
  "addr": 60,               // I2C address (SSD1306)
  "width": 240, "height": 135,
  "x_gap": 40, "y_gap": 53, // ST7789 1.14" RAM offset
  "mirror_x": false, "mirror_y": true, "invert": true,
  "mode": "summary"         // "summary" (all shown sensors, text) | "paged" (text, BOOT cycles pages) | "tiles" (visual grid, groups by page like paged)
}
```

`mode` and per-sensor `show`/`page` apply live. Changing `controller`/pins, or enabling a
previously-disabled display, takes effect after a device reboot.

### Lego

Configuration for the LEGO color-sensor emitter (see [lego-emit.md](lego-emit.md)). The board
appears to a SPIKE Prime / Powered Up hub as a Color Sensor (LPF2 type `0x3D`) and packs the
selected sensor values into its 4×16-bit RGBI payload.

```jsonc
{
  "enabled": false,
  "profile": 0,               // emulated device: 0 = Color Sensor (0x3D), 1 = 3×3 Light Matrix
                              //   (0x40). Selects the type byte the hub binds by; one at a time.
  "debug": false,             // stream the LPF2 handshake/TX/RX trace to the serial console
  "colour_source": 0,         // sensor id (in col_full/as_full mode) driving COLOR/REFLT/RGBI
                              //   (I = the sensor's raw clear count); 0 = generic bit-packing
                              //   via `fields`  (Color Sensor profile)
  "sensor_type": 61,          // LPF2 type byte; informational — the active profile sets it
  "uart_port": 2,             // dedicated UART (UART1 is the aux sensor bus)
  "tx_gpio": 2, "rx_gpio": 1, // LEGO connector: TX -> hub RX, RX <- hub TX
  "baud": 115200,             // operational baud (after the 2400-baud handshake)
  "fields": [                 // target 0 fields packed LSB-first into a 64-bit word
                              //   (R|G<<16|B<<32|I<<48); target 1/2 write a separate byte each
    { "sensor_id": 1,         // source sensor (matches Sensor.id)
      "value_index": 0,       // which decoded value of that sensor
      "bits": 16,             // field width: 4 | 8 | 16 (total across target-0 fields <= 64;
                              //   ignored for target 1/2, always a single byte)
      "signed": false,        // two's-complement encoding (target 0 only)
      "scale": 0.01,          // raw = round((value - offset) / scale); decode = raw*scale + offset
      "offset": 0,
      "target": 0 }           // 0 = RGBI word (default) | 1 = COLOR byte (mode 0) | 2 = REFLT
                              //   byte (mode 1) — COLOR/REFLT bypass RGBI entirely, so
                              //   color()/reflection() see them without rgbi(); only the last
                              //   field targeting a given byte is used
  ]
}
```

`fields` is replaced wholesale when present; the scalar fields keep their value when unset.
Applying a changed `lego` object restarts (or stops) the emitter task without a reboot.

### Discovered (from `scan`)

```jsonc
{
  "bus": "i2c",
  "addr": 118,
  "mux_addr": 112,          // 0 when found on the direct bus
  "channel": 2,             // mux channel, or -1 when direct / n/a
  "guess": "bme280"         // best-effort type hint by known address, or "unknown"
}
```

Each detected **TCA9548A mux** is also reported as its own entry so the UI can confirm it is
wired even with nothing behind it yet. Mux entries carry `kind:"mux"` and are *not* selectable
as sensors:

```jsonc
{ "bus": "i2c", "kind": "mux", "addr": 112, "channels": 8, "guess": "tca9548a" }
```

A board's **built-in display** is reported as a `kind:"display"` entry (SPI panels can't be
auto-detected), and an I2C OLED surfaces as a normal device guessed `ssd1306`. The UI offers
"Use as display" for both:

```jsonc
{ "bus": "spi", "kind": "display", "builtin": true, "controller": "st7789", "cs_index": 5, "guess": "st7789" }
```

For SPI the scan reports each configured CS line that responds to a probe
(`{bus:"spi", cs_index:n, guess}`); for UART it reports the configured port presence.
