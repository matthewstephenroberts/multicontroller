#!/usr/bin/env bash
# Remove firmware and web build artifacts.
source "$(dirname "$0")/_lib.sh"

log "Removing firmware/build"
rm -rf "$FW_DIR/build"
log "Removing web/dist and web/.vite"
rm -rf "$WEB_DIR/dist" "$WEB_DIR/.vite"
log "Removing electron/web-dist and electron/release"
rm -rf "$ELECTRON_DIR/web-dist" "$ELECTRON_DIR/release"
ok "Clean (run with no args; node_modules and sdkconfig are kept)"
