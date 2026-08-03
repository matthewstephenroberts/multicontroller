#!/usr/bin/env bash
# Launch the Vite dev server for the web app (http://localhost:5173).
# Open it in Chrome or Edge — Web Bluetooth is not available in Safari/Firefox.
source "$(dirname "$0")/_lib.sh"

require_npm
cd "$WEB_DIR"
[ -d node_modules ] || { log "Installing web dependencies"; npm install; }
log "Starting Vite dev server → http://localhost:5173  (Ctrl-C to stop)"
exec npm run dev
