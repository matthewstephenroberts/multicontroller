#!/usr/bin/env bash
# Firmware dev loop: build, flash, and open the monitor in one go.
# Usage: dev-firmware.sh [-p PORT] [-b BAUD].  Quit the monitor with Ctrl-].
source "$(dirname "$0")/_lib.sh"

activate_idf
parse_port "$@"
ensure_target
log "Build → flash → monitor (Ctrl-] to exit)"
cd "$FW_DIR"
exec idf.py build flash monitor
