#!/usr/bin/env bash
# Package the MultiController desktop app (Electron wrapper around the web UI) for the current
# OS or a specified target platform. Produces a native installer under electron/release/ —
# .exe (nsis) on Windows, .dmg on macOS, .AppImage/.deb on Linux. electron-builder can't reliably
# cross-build a macOS .dmg from Windows/Linux (needs real macOS tooling), so building "all"
# platforms means running this script on each target OS — see .github/workflows/desktop-build.yml
# for a CI matrix that does exactly that without needing three physical machines.
#
# Usage:
#   build-desktop.sh [options]
#
# Options:
#   --win      Build for Windows (.exe installer)
#   --mac      Build for macOS (.dmg)
#   --linux    Build for Linux (.AppImage and .deb)
#   --dir      Package as unpacked app folder only (skip installer, faster for testing)
#
# Examples:
#   build-desktop.sh              # Build for current OS
#   build-desktop.sh --win        # Build Windows .exe
#   build-desktop.sh --mac        # Build macOS .dmg
#   build-desktop.sh --linux      # Build Linux packages
#   build-desktop.sh --dir        # Build unpacked app (current OS only)
#   build-desktop.sh --win --dir  # Build unpacked app for Windows
source "$(dirname "$0")/_lib.sh"

require_npm

# Parse arguments
TARGET_OS=""
UNPACKAGED_ONLY=false

while [ $# -gt 0 ]; do
  case "$1" in
    --win)   TARGET_OS="win"; shift ;;
    --mac)   TARGET_OS="mac"; shift ;;
    --linux) TARGET_OS="linux"; shift ;;
    --dir)   UNPACKAGED_ONLY=true; shift ;;
    *)       die "Unknown option: $1. Use --win, --mac, --linux, or --dir"; exit 1 ;;
  esac
done

# Build command args
BUILD_ARGS=""
if [ -n "$TARGET_OS" ]; then
  BUILD_ARGS="--$TARGET_OS"
fi
if [ "$UNPACKAGED_ONLY" = true ]; then
  BUILD_ARGS="$BUILD_ARGS --dir"
fi

log "Building web app"
( cd "$WEB_DIR" && [ -d node_modules ] || npm install )
( cd "$WEB_DIR" && npm run build )

log "Copying web build into electron app"
rm -rf "$ELECTRON_DIR/web-dist"
cp -r "$WEB_DIR/dist" "$ELECTRON_DIR/web-dist"

# A major-version bump of electron in package.json against a stale node_modules/package-lock
# (e.g. 32 → 43) can wedge npm's resolver or leave a mismatched binary — detect the mismatch
# and force a clean reinstall instead of letting npm install hang or half-upgrade.
if [ -f "$ELECTRON_DIR/node_modules/electron/package.json" ]; then
  INSTALLED_MAJOR="$(node -p "require('$ELECTRON_DIR/node_modules/electron/package.json').version.split('.')[0]" 2>/dev/null || echo "")"
  WANTED_MAJOR="$(node -p "require('$ELECTRON_DIR/package.json').devDependencies.electron.replace(/[^0-9.]/g,'').split('.')[0]" 2>/dev/null || echo "")"
  if [ -n "$INSTALLED_MAJOR" ] && [ -n "$WANTED_MAJOR" ] && [ "$INSTALLED_MAJOR" != "$WANTED_MAJOR" ]; then
    warn "Installed electron v$INSTALLED_MAJOR != required v$WANTED_MAJOR — clean reinstall"
    rm -rf "$ELECTRON_DIR/node_modules" "$ELECTRON_DIR/package-lock.json"
  fi
fi

log "Installing electron dependencies (electron binary is ~120MB — first install takes a while)"
( cd "$ELECTRON_DIR" && npm install )

if [ "$UNPACKAGED_ONLY" = true ]; then
  if [ -n "$TARGET_OS" ]; then
    log "Packaging unpacked app for $TARGET_OS"
  else
    log "Packaging unpacked app for this OS"
  fi
  ( cd "$ELECTRON_DIR" && npx electron-builder $BUILD_ARGS )
else
  if [ -n "$TARGET_OS" ]; then
    log "Packaging installer for $TARGET_OS"
  else
    log "Packaging installer for this OS"
  fi
  ( cd "$ELECTRON_DIR" && npx electron-builder $BUILD_ARGS )
fi

# macOS: verify the bundle really got sealed (afterPack.js adhoc deep-sign), then clear the
# stale Bluetooth TCC entry. TCC attributes privacy permissions by bundle ID + code signature;
# an unsealed bundle ("Info.plist=not bound"/"Sealed Resources=none") gets CoreBluetooth
# silently denied even with the System Settings toggle on — and because adhoc signatures have
# no stable identity, every rebuild changes the signature, so the previous build's grant no
# longer matches and macOS silently denies WITHOUT re-prompting. Resetting after each build
# forces a fresh permission prompt on next launch, which is exactly what a dev machine wants.
if [ "$(uname)" = "Darwin" ]; then
  APP_BUNDLE="$(find "$ELECTRON_DIR/release" -maxdepth 2 -name "*.app" -type d | head -1)"
  if [ -n "$APP_BUNDLE" ]; then
    log "Verifying code signature seal"
    SIGN_INFO="$(codesign -dvv "$APP_BUNDLE" 2>&1)"
    if echo "$SIGN_INFO" | grep -q "Info.plist=not bound"; then
      die "App bundle is NOT properly signed (Info.plist not bound) — Bluetooth will be silently denied by TCC. Check electron/afterPack.js ran."
    fi
    echo "$SIGN_INFO" | grep -E "^Identifier=|^Signature=" | sed 's/^/    /'
    ok "Bundle sealed"

    # TCC service name is the kTCCService* constant minus the prefix: Bluetooth is
    # kTCCServiceBluetoothAlways → "BluetoothAlways" (plain "Bluetooth" is not a valid service).
    log "Resetting Bluetooth permission (forces a fresh prompt for this new build)"
    tccutil reset BluetoothAlways com.multicontroller.app >/dev/null 2>&1 \
      && ok "Bluetooth TCC entry cleared — allow the prompt on next launch" \
      || warn "tccutil reset failed (no existing entry?) — if no Bluetooth prompt appears on launch, run: tccutil reset BluetoothAlways com.multicontroller.app"
  else
    warn "No .app bundle found under electron/release — skipping signature check"
  fi
fi

ok "Desktop build complete — see electron/release/"
