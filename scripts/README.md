# Scripts

Helper scripts for building and developing both halves of the project. Run them from
anywhere; each resolves the repo root itself.

| Script               | What it does                                                            |
|----------------------|-------------------------------------------------------------------------|
| `install-esp-idf.sh` | Install ESP-IDF + toolchain (macOS / Linux).                           |
| `install-esp-idf.ps1`| Install ESP-IDF + toolchain (Windows PowerShell).                      |
| `build-all.sh`       | Build firmware **and** frontend.                                        |
| `build-firmware.sh`  | Build the ESP32-S3 firmware (sets target `esp32s3` on first run).       |
| `build-frontend.sh`  | `npm install` (if needed) + typecheck + `vite build` → `web/dist`.      |
| `flash.sh`           | Build and flash the firmware to the board.                             |
| `monitor.sh`         | Open the device serial monitor (its log terminal). Quit with `Ctrl-]`. |
| `dev-firmware.sh`    | Hardware dev loop: build → flash → monitor in one command.             |
| `dev-frontend.sh`    | Launch the Vite dev server at http://localhost:5173.                   |
| `build-desktop.sh`   | Build the web app + package the Electron desktop app for this OS.      |
| `dev-desktop.sh`     | Run the desktop app against the live Vite dev server (hot-reload).     |
| `clean.sh`           | Remove `firmware/build` and `web/dist`.                                |

## Install ESP-IDF (one-time)

```bash
# macOS / Linux — clones esp-idf (default v6.0.2) and installs the esp32s3 toolchain
./scripts/install-esp-idf.sh

# pin a different version / install location / extra chips, or force a Python:
IDF_VERSION=release/v5.4 IDF_TARGETS="esp32s3 esp32c3" ./scripts/install-esp-idf.sh
IDF_PYTHON=python3.13 ./scripts/install-esp-idf.sh     # use a specific interpreter
```

```powershell
# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -File scripts\install-esp-idf.ps1
```

ESP-IDF supports **Python 3.10–3.14** (3.14 is fully supported), so the installers use the
system `python3` by default. The default ESP-IDF version is **v6.0.2** (latest stable) — set
`IDF_VERSION=release/v5.4` (or a tag like `v5.4.3`) if you want the 5.4 line instead. After
install, the build/flash scripts pick up ESP-IDF automatically — no need to source anything
yourself for those.

**TLS certificates:** python.org's macOS Python ships without a linked CA bundle, which makes
the toolchain download fail with `CERTIFICATE_VERIFY_FAILED`. The installers detect this and
point `urllib` at certifi's bundle (`SSL_CERT_FILE`) automatically for the install. If it still
fails, run `open "/Applications/Python 3.14/Install Certificates.command"` (macOS) or
`python3 -m pip install --upgrade certifi`, then re-run.

## Examples

```bash
# One-time / CI: build everything
./scripts/build-all.sh

# Firmware: flash a specific port and watch the log
./scripts/flash.sh -p /dev/cu.usbmodem1101
./scripts/monitor.sh -p /dev/cu.usbmodem1101

# Firmware: the usual edit-build-flash-watch loop
./scripts/dev-firmware.sh            # auto-detects the port

# Frontend: live dev server (open in Chrome/Edge)
./scripts/dev-frontend.sh

# Desktop app: iterate against the live dev server
./scripts/dev-desktop.sh

# Desktop app: package a native installer for this OS → electron/release/
./scripts/build-desktop.sh
```

## Desktop app (Electron)

The `electron/` directory wraps the same web UI in a native window — no browser, no dev server,
just a double-clickable app. It only exists to solve the Web Bluetooth device-picker gap
(Electron doesn't show one natively) and to package installers; the UI code itself is unchanged.

- `./scripts/build-desktop.sh` builds the web app and packages it for **the OS you run it on**
  (`.exe`/nsis on Windows, `.dmg` on macOS, `.AppImage`/`.deb` on Linux) into `electron/release/`.
  Pass `--dir` to skip installer packaging and just produce an unpacked app folder (faster for
  local testing).
- electron-builder can't reliably cross-build a macOS `.dmg` from Windows/Linux (or vice versa
  for proper code-signing), so **building all three platforms means running this script on each
  OS** — or trigger `.github/workflows/desktop-build.yml` (Actions tab → "Build desktop app" →
  Run workflow), which builds all three on GitHub's own runners and uploads each as an artifact.

## Serial port

The firmware scripts accept `-p/--port <device>` and `-b/--baud <rate>`; without `-p`,
`idf.py` auto-detects the port. List ports with `ls /dev/cu.usb*` (macOS).

## ESP-IDF lookup

The firmware scripts need `idf.py`. If it isn't already on your `PATH`, they source
`export.sh` from common install locations (`$IDF_PATH`, `~/esp/...`,
`~/.espressif/frameworks/...`). If none are found they print how to install ESP-IDF.
Inside the VSCode Espressif extension's terminal, `idf.py` is already on `PATH`.

## Windows note

`install-esp-idf.ps1` is native PowerShell. The other helpers are Bash — on Windows run them
from **Git Bash** or **WSL**, or just use `idf.py` directly after sourcing the environment:

```powershell
. "$HOME\esp\esp-idf\export.ps1"
idf.py -C firmware build flash monitor      # firmware
npm --prefix web run dev                     # frontend
```
