#!/usr/bin/env bash
# Build the ESP32-S3 firmware (sets target esp32s3 on first run).
source "$(dirname "$0")/_lib.sh"

activate_idf
ensure_target
log "Building firmware"
( cd "$FW_DIR" && idf.py build )
ok "Firmware built → $FW_DIR/build"
