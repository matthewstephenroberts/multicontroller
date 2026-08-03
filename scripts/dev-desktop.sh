#!/usr/bin/env bash
# Run the desktop app against the live Vite dev server, so UI edits hot-reload without
# repackaging the Electron app each time. Usage: dev-desktop.sh
source "$(dirname "$0")/_lib.sh"

require_npm

( cd "$WEB_DIR" && [ -d node_modules ] || npm install )
log "Starting Vite dev server → http://localhost:5173"
( cd "$WEB_DIR" && npm run dev >/dev/null 2>&1 & echo $! > /tmp/mc-dev-server.pid )
trap '[ -f /tmp/mc-dev-server.pid ] && kill "$(cat /tmp/mc-dev-server.pid)" 2>/dev/null; rm -f /tmp/mc-dev-server.pid' EXIT

log "Waiting for dev server to come up"
for _ in $(seq 1 30); do
  curl -s -o /dev/null "http://localhost:5173" && break
  sleep 0.5
done

( cd "$ELECTRON_DIR" && [ -d node_modules ] || npm install )
log "Launching Electron"
( cd "$ELECTRON_DIR" && MC_DEV_SERVER_URL="http://localhost:5173" npx electron . )
