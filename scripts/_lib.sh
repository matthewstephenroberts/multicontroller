# _lib.sh — shared helpers for MultiController scripts. Source this; do not execute.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FW_DIR="$REPO_ROOT/firmware"
WEB_DIR="$REPO_ROOT/web"
ELECTRON_DIR="$REPO_ROOT/electron"

# --- logging ---
if [ -t 1 ]; then
  c_blue='\033[0;34m'; c_green='\033[0;32m'; c_yellow='\033[0;33m'; c_red='\033[0;31m'; c_reset='\033[0m'
else
  c_blue=''; c_green=''; c_yellow=''; c_red=''; c_reset=''
fi
log()  { printf "${c_blue}▶ %s${c_reset}\n" "$*"; }
ok()   { printf "${c_green}✔ %s${c_reset}\n" "$*"; }
warn() { printf "${c_yellow}! %s${c_reset}\n" "$*"; }
die()  { printf "${c_red}✗ %s${c_reset}\n" "$*" >&2; exit 1; }

# --- ESP-IDF ---
# Make idf.py available, sourcing export.sh from common install locations if needed.
activate_idf() {
  command -v idf.py >/dev/null 2>&1 && return 0

  local candidates=(
    "${IDF_PATH:-}/export.sh"
    "$HOME/esp/esp-idf/export.sh"
    "$HOME/esp/v6.0/esp-idf/export.sh"
    "$HOME/esp/v5.5/esp-idf/export.sh"
    "$HOME/esp/v5.4/esp-idf/export.sh"
    "$HOME/.espressif/frameworks/esp-idf-v6.0/export.sh"
    "$HOME/.espressif/frameworks/esp-idf-v5.5/export.sh"
    "$HOME/.espressif/frameworks/esp-idf-v5.4/export.sh"
  )
  local e
  for e in "${candidates[@]}"; do
    [ -n "$e" ] && [ -f "$e" ] || continue
    log "Activating ESP-IDF: $e"
    set +u
    # shellcheck disable=SC1090
    . "$e" >/dev/null 2>&1 || true
    set -u
    command -v idf.py >/dev/null 2>&1 && { ok "ESP-IDF ready"; return 0; }
  done

  die "idf.py not found. Run ./scripts/install-esp-idf.sh (or the Espressif VSCode extension), or set IDF_PATH, then retry."
}

# Parse common serial options into the environment idf.py honours.
#   -p|--port <dev>   -> ESPPORT     (default: auto-detect)
#   -b|--baud <rate>  -> ESPBAUD
parse_port() {
  while [ $# -gt 0 ]; do
    case "$1" in
      -p|--port) export ESPPORT="$2"; shift 2 ;;
      -b|--baud) export ESPBAUD="$2"; shift 2 ;;
      *) shift ;;
    esac
  done
  [ -n "${ESPPORT:-}" ] && log "Serial port: $ESPPORT" || true
}

require_npm() { command -v npm >/dev/null 2>&1 || die "npm not found — install Node 18+."; }

# Set the ESP32-S3 target on first build only (set-target rewrites sdkconfig).
# Also detect if the build directory was configured for a different project path.
ensure_target() {
  # A build dir left behind by a failed configure (no CMakeCache.txt) makes
  # set-target's fullclean refuse to run. Clear that junk so we can recover.
  if [ -d "$FW_DIR/build" ] && [ ! -f "$FW_DIR/build/CMakeCache.txt" ]; then
    warn "Removing incomplete build directory"
    rm -rf "$FW_DIR/build"
  fi

  # Check if build directory exists but was configured for a different source path (e.g. project moved).
  # CMakeCache.txt contains SOURCE_DIR=/path/to/old/location — if it doesn't match current FW_DIR, clean it.
  if [ -f "$FW_DIR/build/CMakeCache.txt" ]; then
    local cached_source_dir
    cached_source_dir=$(grep -m1 "^CMAKE_HOME_DIRECTORY" "$FW_DIR/build/CMakeCache.txt" | cut -d'=' -f2 || true)
    if [ -n "$cached_source_dir" ] && [ "$cached_source_dir" != "$FW_DIR" ]; then
      warn "Build directory was configured for a different project path"
      warn "  Cached: $cached_source_dir"
      warn "  Actual: $FW_DIR"
      log "Cleaning build directory"
      ( cd "$FW_DIR" && idf.py fullclean )
    fi
  fi

  if [ ! -f "$FW_DIR/sdkconfig" ]; then
    log "First build — setting target esp32s3"
    ( cd "$FW_DIR" && idf.py set-target esp32s3 )
  fi
}
