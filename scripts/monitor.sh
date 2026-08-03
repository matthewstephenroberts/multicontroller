#!/usr/bin/env bash
# Open the serial monitor (the device's log terminal).  Usage: monitor.sh [-p PORT]
# Quit the monitor with Ctrl-].
source "$(dirname "$0")/_lib.sh"

activate_idf
parse_port "$@"
log "Opening serial monitor — press Ctrl-] to exit"
cd "$FW_DIR"
exec idf.py monitor
