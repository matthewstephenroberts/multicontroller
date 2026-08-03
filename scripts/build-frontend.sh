#!/usr/bin/env bash
# Typecheck and build the React/Vite web app into web/dist.
source "$(dirname "$0")/_lib.sh"

require_npm
cd "$WEB_DIR"
[ -d node_modules ] || { log "Installing web dependencies"; npm install; }
log "Building web app"
npm run build
ok "Web build → $WEB_DIR/dist"
