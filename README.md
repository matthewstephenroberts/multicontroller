# MultiController

**Give retiring LEGO Education sets new life with modern sensors** — an M5Stack AtomS3 + Atomic Motion Base sensor hub that connects to LEGO SPIKE Prime and Powered Up hubs over LPF2.

Configure any mix of M5Stack Unit sensors (colour, distance, 8-angle, step16, and more) from your phone or laptop, no reprogramming needed. Perfect for rescuing old LEGO kits in the classroom.

**[📖 Read the full Hackster.io project guide](https://www.hackster.io/matthew-stephen-roberts/multicontroller-keeping-retiring-lego-education-sets-alive-0b24bf)** — step-by-step build instructions, wiring diagrams, and classroom tips.

---

## Technical overview

An **ESP32-S3** sensor hub whose sensor set is **not hard-coded** — it is configured at
runtime over **Bluetooth LE** and persisted in **NVS**. Sensors can be attached over **I2C**
(through a **TCA9548A** mux, **PaHub v2.1**, or **Grove Y Cable**), **SPI** (up to 5 chip-select lines), or **UART**. A **React + Vite**
web app (Web Bluetooth, Chrome/Edge desktop) discovers the device, scans its buses, lets you
choose which sensors to monitor and at what poll rate, saves that to the device, and shows a
live dashboard.

```
┌──────────────┐   Bluetooth LE    ┌──────────────────────────────┐
│  Web app     │  (NUS-style GATT, │   ESP32-S3 firmware          │
│  React+Vite  │◀─ length-framed ─▶│   NimBLE · NVS · scheduler   │
│  Web BLE     │      JSON)        │                              │
└──────────────┘                   │  I2C ─▶ TCA9548A ─▶ sensors  │
                                   │  SPI ─▶ CS0..CS4  ─▶ sensors │
                                   │  UART ───────────▶  sensor   │
                                   └──────────────────────────────┘
```

## Layout

| Path                | What                                                            |
|---------------------|-----------------------------------------------------------------|
| `firmware/`         | ESP-IDF project (ESP32-S3, NimBLE).                             |
| `web/`              | React + Vite + TypeScript Web Bluetooth client.                |
| `docs/user-manual.html` / `.pdf` | End-user manual — setup, wiring, and every feature, written for non-developers (in `docs/assets/`). |
| `docs/ble-protocol.md` | The BLE wire contract — single source of truth.             |
| `docs/wiring.md`    | Pin map, TCA9548A wiring, example sensors.                      |
| `docs/sensors.md`   | Sensor types & convert modes reference (env/IMU/distance/CO2/generic recipe). |
| `docs/lego-emit.md` | LEGO colour-sensor / 3×3-matrix emitter (LPF2 to a SPIKE/PUP hub). |
| `docs/gpio-sensors.md` | The 5 DA pins as digital/analog sensors.                     |
| `docs/colour-calibration.md` | Teachable colour palette (per-colour + custom + resets).  |
| `docs/hid-gamepad.md` | Bluetooth game controller (Xbox Series) → sensor → LEGO hub.  |
| `scripts/`          | Build / flash / monitor / dev helpers — see `scripts/README.md`. |

## Prerequisites

- **ESP-IDF v6.0.2** (latest stable; the 5.4 line also works — set `IDF_VERSION`) — install it one of two ways:
  - **CLI**: `./scripts/install-esp-idf.sh` (macOS/Linux) or
    `scripts\install-esp-idf.ps1` (Windows). ESP-IDF supports Python 3.10–3.14 (3.14 included),
    so the installer uses your system `python3` directly.
  - **VSCode**: the **Espressif extension** (`espressif.esp-idf-extension`), which ships its own Python.
- **Node 18+ / npm** for the web app (Node 22 is installed here).
- A **Chromium-based browser** (Chrome or Edge) for Web Bluetooth — *not* Safari/Firefox.

## Getting started

**New to MultiController?** Start here:
- 📖 [**Installation Guide**](INSTALLATION.md) — How to install pre-built firmware or desktop app
- 🛠️ [**Development Guide**](DEVELOPMENT.md) — For developers who want to modify or extend the code
- 🤝 [**Contributing**](CONTRIBUTING.md) — How to add sensor drivers or improve the project
- 📚 [**Full Hackster.io guide**](https://www.hackster.io/matthew-stephen-roberts/multicontroller-keeping-retiring-lego-education-sets-alive-0b24bf) — Step-by-step build instructions

## Quick start (helper scripts)

```bash
./scripts/build-all.sh        # build firmware + frontend
./scripts/dev-firmware.sh     # build → flash → serial monitor (the device terminal)
./scripts/dev-frontend.sh     # launch the Vite dev server at localhost:5173
```

The firmware scripts find `idf.py` automatically (sourcing ESP-IDF's `export.sh` if needed).
See [`scripts/README.md`](scripts/README.md) for the full list. The manual commands below do the
same thing.

## Build & flash the firmware

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor      # e.g. -p /dev/cu.usbmodemXXXX
```

On boot the device loads sensor config from NVS (seeds an empty config the first time) and
advertises as **`MultiController`**.

> Check your module's flash size and adjust `firmware/partitions.csv` if it is not ≥4 MB.

## Run the web app

```bash
cd web
npm install
npm run dev          # serves on http://localhost:5173 (Web BLE works on localhost)
```

Open in **Chrome/Edge**, then:

1. **Connect** — click *Scan for device* and pick `MultiController`.
2. **Scan sensors** — enumerate attached I2C devices (probed across every mux channel) plus the
   configured SPI CS lines / UART port.
3. **Configure** — choose sensors, set type, I2C mux channel **or** SPI CS index (0–4), and poll
   rate. **Save** persists to the device's NVS.
4. **Dashboard** — watch live readings refresh at each sensor's poll rate.

See [`docs/ble-protocol.md`](docs/ble-protocol.md) for the message format and
[`docs/wiring.md`](docs/wiring.md) for hardware.

## Desktop app (no browser needed)

`electron/` packages the same web app into a native double-clickable app for Windows/macOS/Linux
— no dev server, no browser tab, just an icon to launch. Web Bluetooth is Chromium-only (no iOS,
no Safari, no Firefox), so that platform gap still applies here regardless of packaging.

```bash
./scripts/build-desktop.sh     # → electron/release/ (installer for the OS you run this on)
./scripts/dev-desktop.sh       # iterate against the live Vite dev server
```

See [`scripts/README.md`](scripts/README.md#desktop-app-electron) for cross-platform build
details (building all three OS installers needs either three machines or the included GitHub
Actions workflow).

## Support MultiController development

If you'd like to support development or fund support for additional M5Stack sensors:

- ⭐ [GitHub Sponsors](https://github.com/sponsors/matthewstephenroberts)
- ☕ [Buy Me a Coffee](https://buymeacoffee.com/matthewstephenroberts)

**Current priorities:**
- Support for AS7341 spectral sensor
- BME680 environmental sensor (temperature, humidity, pressure, gas)
- Multiple LPF2 hub connections from one device

Have a sensor you'd like to see supported? Donations help prioritize new drivers.

## Contributing

MultiController thrives on community contributions. Whether it's a new sensor driver, bug reports, documentation, or classroom feedback — all are welcome.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for:
- How to add a new sensor driver
- How to report bugs
- How to improve documentation
- Code style and pull request process

Please also read our [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — we're building this for classrooms and learners of all ages.

## License & Educational Intent

**MIT License** — see [`LICENSE`](LICENSE) for full legal text.

This project is released under the MIT license to enable broad adoption and community contribution. However, it is **designed and maintained with educational institutions and learners at the core**.

### Our Intent

MultiController exists to:
- **Rescue retiring LEGO Education sets** and keep them useful in classrooms
- **Lower barriers to sensor robotics** through runtime configuration (no reprogramming needed)
- **Support educators** with free, open-source tools for science and engineering education
- **Empower students** to experiment with sensors without licensing costs

### How We Hope You'll Use It

- 🏫 **Educators**: Use freely in your classrooms, adapt for your students' needs
- 🤝 **Commercial users**: You're welcome to use this in products — if they succeed, please support development
- 👨‍💻 **Developers**: Build on it, improve it, share your enhancements with the community
- 🌍 **Non-profits**: Use for free — this is your tool

### What We Ask

While MIT allows commercial use without restriction, we encourage:
1. **Credit the project** — acknowledge MultiController and the MIT license in your work
2. **Support development** — if your product succeeds commercially, fund new sensor drivers and hardware testing via donations
3. **Share improvements** — if you add sensor drivers or features, consider sharing them via pull requests
4. **Foster community** — help other educators and learners use this tool

See [`LICENSE-INTENT.md`](LICENSE-INTENT.md) for more details on our values and vision.
