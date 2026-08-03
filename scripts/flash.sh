#!/usr/bin/env bash
# Build and flash the firmware to the board.  Usage: flash.sh [-p PORT] [-b BAUD]
source "$(dirname "$0")/_lib.sh"

activate_idf
parse_port "$@"
ensure_target
log "Building and flashing"
( cd "$FW_DIR" && idf.py build flash )
ok "Flashed"
