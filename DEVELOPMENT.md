# Development guide

For developers who want to modify the firmware, web app, or add new sensor drivers.

## Quick start

```bash
# One-time setup
./scripts/install-esp-idf.sh        # Install ESP-IDF toolchain (macOS/Linux)
# or scripts\install-esp-idf.ps1    # (Windows PowerShell)

# Clone and enter the repo
git clone https://github.com/matthewstephenroberts/multicontroller.git
cd MultiController

# Build everything
./scripts/build-all.sh

# Or develop on individual parts:
./scripts/dev-firmware.sh           # Build → flash → monitor device serial
./scripts/dev-frontend.sh           # Start Vite dev server (localhost:5173)
./scripts/dev-desktop.sh            # Build and run desktop app with live reload
```

See [`scripts/README.md`](scripts/README.md) for all available helper scripts.

## Firmware (ESP32-S3, C)

**Location:** `firmware/`

### Prerequisites
- ESP-IDF v6.0.2 or v5.4
- Python 3.10+
- USB cable to your AtomS3

### Build and flash

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The device will boot, load NVS config, and advertise as `MultiController` over Bluetooth.

### Adding a new sensor driver

1. **Create driver file:** `firmware/components/sensor/drivers/drv_<sensor_name>.c`
2. **Implement `sensor_driver_t` interface:**
   ```c
   sensor_driver_t my_sensor_driver = {
       .type = SENSOR_TYPE_CUSTOM,
       .probe = probe_my_sensor,
       .read = read_my_sensor,
       .close = close_my_sensor,
   };
   ```
3. **Register in** `firmware/components/sensor/sensor.c` (add to `drivers[]` array)
4. **Document in** `docs/sensors.md`
5. **Test** with hardware if possible
6. **Submit a PR** with example output and wiring notes

See `firmware/components/sensor/drivers/drv_vl53l1x.c` for a complete example.

### Key firmware files

- `firmware/main/` — ESP-IDF app entry point, board setup
- `firmware/main/board_config.h` — Choose your board (ATOMS3_LITE / ATOMS3R)
- `firmware/main/boards/` — Per-board pin maps and initialization
- `firmware/components/ble_svc/` — Bluetooth protocol (NUS-style GATT)
- `firmware/components/sensor/` — Sensor drivers and scheduler
- `firmware/components/bus_scan/` — I2C bus discovery
- `firmware/components/lego_emit/` — LPF2 LEGO hub emitter
- `firmware/sdkconfig.defaults` — ESP-IDF build config

## Web app (React + Vite, TypeScript)

**Location:** `web/`

### Prerequisites
- Node 18+ / npm

### Development

```bash
cd web
npm install
npm run dev                 # Start Vite dev server (localhost:5173)
```

While running:
- Open http://localhost:5173 in Chrome/Edge
- Changes to `.tsx` files hot-reload automatically
- Serial console is available in browser if you flashed the device

### Key files

- `web/src/App.tsx` — Main app component
- `web/src/ble/bleClient.ts` — Bluetooth LE communication (Web Bluetooth API)
- `web/src/ble/protocol.ts` — Message parsing (mirrors `docs/ble-protocol.md`)
- `web/src/components/` — Reusable UI components
- `web/src/types.ts` — Shared TypeScript types

### Building for production

```bash
npm run build               # Creates optimized bundle in dist/
```

## Desktop app (Electron)

**Location:** `electron/`

### Development

```bash
./scripts/dev-desktop.sh    # Launches Electron app with live reload
```

Changes to the web app source automatically reload in Electron.

### Building installers

```bash
./scripts/build-desktop.sh  # Creates installer for your OS
```

Output in `electron/release/`:
- **Windows:** `.exe` installer
- **macOS:** `.dmg` disk image
- **Linux:** `.deb` and `.AppImage`

To build all three platforms, use the GitHub Actions workflow (see `.github/workflows/`).

## Testing

### Manual testing

1. **Flash firmware:** `./scripts/dev-firmware.sh`
2. **Start web app:** `./scripts/dev-frontend.sh`
3. **Test in browser:** http://localhost:5173
4. **Use with Vite dev tools** — React DevTools, Network tab

### Integration testing (for developers)

The web app has a **demo mode** that simulates a device entirely in the browser:
- No physical hardware needed
- No Bluetooth permission required
- Test UI/UX without flashing

To enable: Set `VITE_DEMO_MODE=true` in `.env.local`

## Code style

### Firmware (C)
- Follow ESP-IDF conventions
- Use `clang-format` if available: `clang-format -i firmware/main/*.c`
- Keep functions under 100 lines
- Comment *why*, not *what*

### Web app (TypeScript/React)
- `npm run lint` should pass
- Use functional components + hooks
- Keep components small and focused
- Prefer explicit types over `any`

### Commits
- Describe *why* the change is needed, not just *what* changed
- Good: "Support VL53L0X variant (shares register map with VL53L1X)"
- Bad: "Add VL53L0X sensor"

## Debugging

### Firmware
```bash
./scripts/dev-firmware.sh   # Serial monitor automatically opens
# Press Ctrl+T, Ctrl+] to quit
```

Common debug output:
- `I (boot)` — Boot messages
- `E (sensor)` — Sensor errors
- `D (ble)` — Bluetooth debug

### Web app
- Open DevTools (F12 in Chrome)
- Console tab shows app logs
- Network tab shows Bluetooth messages (if in dev mode)
- React DevTools extension recommended

## Project structure

```
MultiController/
├── firmware/              # ESP32-S3 firmware (C + ESP-IDF)
├── web/                   # React + Vite web app
├── electron/              # Electron desktop app
├── docs/                  # User/technical documentation
├── scripts/               # Build and helper scripts
├── .github/workflows/     # GitHub Actions CI/CD
└── README.md              # You are here
```

## Releases

Maintainers: To cut a new release:

1. Update version in `firmware/main/CMakeLists.txt`, `web/package.json`, `electron/package.json`
2. Update `CHANGELOG.md` (create if missing)
3. Commit with message `Release vX.Y.Z`
4. Tag: `git tag vX.Y.Z`
5. Push: `git push origin main --tags`
6. GitHub Actions will build and create a release automatically
7. Download binaries from [Releases](https://github.com/matthewstephenroberts/multicontroller/releases) and attach to the GitHub release

## Getting help

- Check existing [issues](https://github.com/matthewstephenroberts/multicontroller/issues) and [discussions](https://github.com/matthewstephenroberts/multicontroller/discussions)
- Open a new issue with details: hardware, steps to reproduce, error messages
- For design questions, start a discussion first

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for:
- How to submit pull requests
- Community guidelines
- Sensor driver checklist

Thanks for contributing! 💙
