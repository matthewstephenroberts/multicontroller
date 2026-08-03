#!/usr/bin/env bash
# Build both firmware and frontend.
source "$(dirname "$0")/_lib.sh"

"$SCRIPT_DIR/build-firmware.sh"
"$SCRIPT_DIR/build-frontend.sh"
ok "All builds complete"
