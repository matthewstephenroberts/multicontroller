# Vendored third-party sources

The files in this directory are Bosch Sensortec's official sensor API drivers, fetched verbatim
(unmodified) from their public GitHub repositories:

- `bmi2.c`, `bmi2.h`, `bmi2_defs.h`, `bmi270.c`, `bmi270.h` — from
  https://github.com/boschsensortec/BMI270_SensorAPI (branch `master`)
- `bmm150.c`, `bmm150.h`, `bmm150_defs.h` — from
  https://github.com/boschsensortec/BMM150_SensorAPI (branch `master`)

Both repositories are licensed **BSD-3-Clause** (Copyright (c) Bosch Sensortec GmbH). See the
license header at the top of each file. These are included unmodified rather than hand-reimplemented
because `bmi270_init()` requires uploading Bosch's proprietary ~8KB binary configuration blob
(`bmi270_config_file[]`, in `bmi270.c`) for the chip to produce any valid data at all — that data
is only trustworthy from the authoritative source.

The `imu.c`/`imu.h` files one directory up are this project's own glue code (I2C read/write/delay
callbacks and a small polling API), written specifically for MultiController and following the
call sequence in Bosch's own `bmi270_examples/read_aux_data_mode/read_aux_data_mode.c` reference
example (BMI270 host + BMM150 reached through BMI270's auxiliary I2C port).
