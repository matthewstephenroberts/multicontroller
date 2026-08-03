# Installation Guide

Three ways to get MultiController running:

## 1. Pre-built firmware (easiest for most users)

If you just want to use MultiController without compiling:

1. Download the latest `.bin` file from [Releases](https://github.com/matthewstephenroberts/multicontroller/releases)
2. Install [esptool.py](https://github.com/espressif/esptool):
   ```bash
   pip install esptool
   ```
3. Connect your AtomS3/AtomS3R to your computer via USB
4. Flash the firmware:
   ```bash
   esptool.py -p /dev/ttyUSB0 write_flash 0x0 MultiController.bin
   ```
   (On Mac, try `/dev/tty.usbserial-*` or `/dev/cu.usbmodem*`)
   (On Windows, use `COM3` or `COM4`)

5. Power on the device — it will advertise as `MultiController` over Bluetooth

**That's it!** Skip to "Use the web app" below.

## 2. Desktop app (Windows/macOS/Linux)

Download the installer from [Releases](https://github.com/matthewstephenroberts/multicontroller/releases):

- **Windows:** `MultiController-Setup.exe`
- **macOS:** `MultiController.dmg`
- **Linux:** `multicontroller_X.X.X_amd64.deb` or `.AppImage`

Install and launch. The app will:
- Connect to your AtomS3 over Bluetooth
- Show the sensor configuration dashboard
- Save settings to the device

No browser needed, no dev server required.

## 3. Build from source (for developers)

See [README.md](README.md#build--flash-the-firmware) and [CONTRIBUTING.md](CONTRIBUTING.md#development-setup).

```bash
./scripts/dev-firmware.sh       # Build and flash to device
./scripts/dev-frontend.sh       # Start web app locally
./scripts/dev-desktop.sh        # Iterate on desktop app
```

## Use the web app

After flashing the firmware:

1. Open **Chrome or Edge** (not Safari or Firefox — Web Bluetooth is Chromium-only)
2. Go to `http://localhost:5173` if running locally, or to the hosted version
3. Click **Scan for device** and pick `MultiController`
4. Click **Scan I2C Bus** to find connected sensors
5. Configure each sensor (type, poll rate) and click **Save**
6. Watch the live dashboard update with sensor readings

## Troubleshooting

**Device won't connect (Bluetooth)**
- Make sure you're using Chrome or Edge
- Check that the AtomS3 is powered on
- Try clicking "Reset" in the web app

**Sensor doesn't appear in scan**
- Make sure the sensor cable is fully plugged into Port A (Grove port)
- Try unplugging and re-plugging the sensor
- Power off the Motion Base, wait 10 seconds, power back on

**Flashing fails**
- Check that the USB cable is plugged in both ends (some cables are charge-only)
- Try a different USB cable
- On Mac, try each port in `/dev/tty.usbserial-*`
- Make sure you chose the right board (AtomS3 Lite vs. AtomS3R)

**Still stuck?** Open an [issue](https://github.com/matthewstephenroberts/multicontroller/issues) and describe:
- Your hardware (AtomS3 Lite / AtomS3R, Motion Base, sensors)
- What you tried
- The error message or behavior

## Next steps

- [📖 Full Hackster.io guide](https://www.hackster.io/) — building, wiring, and classroom setup
- [`docs/ble-protocol.md`](docs/ble-protocol.md) — Bluetooth message format (for developers)
- [`docs/wiring.md`](docs/wiring.md) — Pin maps and detailed hardware connections
- [`docs/sensors.md`](docs/sensors.md) — Supported sensors and their properties
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — How to add new sensors or improve the project
