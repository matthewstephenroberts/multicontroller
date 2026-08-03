# Contributing to MultiController

Thanks for interest in contributing! MultiController thrives on community contributions, especially new sensor drivers and classroom improvements.

## Ways to contribute

### 1. Add a new sensor driver (most valuable)

Want to see support for another M5Stack sensor or I2C device? A new sensor driver is the highest-value contribution.

**Before you start:**
- Check [firmware/components/sensor/](firmware/components/sensor/) to see if the sensor already exists
- Open an issue describing the sensor and what you'd like to add
- Discuss the I2C address and any special handling needed

**To add a sensor:**

1. Create a new driver file in `firmware/components/sensor/drivers/` named `drv_<sensor_name>.c`
2. Implement the `sensor_driver_t` interface (see existing drivers like `drv_vl53l1x.c` for examples)
3. Register it in `firmware/components/sensor/sensor.c`
4. Add documentation to `docs/sensors.md`
5. Test with hardware if possible
6. Submit a pull request

**Example sensor drivers to reference:**
- `drv_tcs34725.c` — Colour sensor (TCS34725)
- `drv_vl53l1x.c` — Distance sensor (VL53L1X)
- `drv_m5_8angle.c` — Multi-channel sensor (8Angle Unit)

### 2. Report bugs or suggest features

- Found a bug? Open an issue with:
  - What happened
  - What you expected
  - Your hardware setup (AtomS3 Lite / AtomS3R, Motion Base version, sensors)
  - Steps to reproduce if possible

- Have a feature idea? Describe:
  - What it should do
  - Why it's useful
  - How it might work

### 3. Improve documentation

- Typos or unclear instructions in `docs/` or README
- Translation to another language
- Better wiring diagrams or examples
- Classroom-friendly tutorials

### 4. Help with classroom adoption

- You're using this in a classroom? Share your setup
- Curriculum ideas or lesson plans
- Feedback on ease of use for students
- Safety or accessibility improvements

## Development setup

```bash
# Install prerequisites
./scripts/install-esp-idf.sh       # macOS/Linux
# or scripts\install-esp-idf.ps1   # Windows

# Build firmware
./scripts/dev-firmware.sh

# Run web app
./scripts/dev-frontend.sh          # http://localhost:5173

# Build desktop app
./scripts/dev-desktop.sh
```

See `scripts/README.md` for more details.

## Code style

- **Firmware (C):** Follow ESP-IDF conventions. Format with `clang-format` if available.
- **Web app (TypeScript/React):** `npm run lint` should pass.
- **Commit messages:** Describe *why*, not just *what* — "Support VL53L0X variant (shares register map)" is better than "Add VL53L0X".

## Pull request process

1. Fork the repo and create a branch: `git checkout -b add-bme680-sensor`
2. Make your changes and test them
3. Commit with a clear message
4. Push to your fork and create a pull request
5. Link any related issues
6. Describe what you changed and why

A maintainer will review and merge or request changes.

## Funding sensor support

Want to fund development of a specific sensor? See the **Support** section in `README.md` for donation options. When you donate, mention which sensor you'd like to see supported, and it gets prioritized.

## Questions?

- Check `docs/` for technical details
- Open an issue and tag it `question`
- Check existing issues — your question may be answered

## Code of Conduct

See `CODE_OF_CONDUCT.md` — be kind and respectful. We're building this for classrooms and learners.

Thanks for contributing! 🎉
