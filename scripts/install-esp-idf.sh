#!/usr/bin/env bash
# Install ESP-IDF (framework + toolchain) on macOS / Linux.
#
# Env overrides:
#   IDF_VERSION    git ref to check out          (default: v6.0.2)
#   IDF_CLONE_DIR  where to clone esp-idf         (default: ~/esp/esp-idf)
#   IDF_TARGETS    chip(s) to install tools for   (default: esp32s3)
#   IDF_PYTHON     python interpreter to use      (default: system python3)
#
# ESP-IDF supports Python 3.10–3.14 (3.14 is fully supported on Linux/macOS/Windows),
# so the system python3 is used by default. The default targets v6.0.2 (latest stable);
# override with e.g. IDF_VERSION=v5.4.3 or IDF_VERSION=release/v5.4 for the 5.4 line.
source "$(dirname "$0")/_lib.sh"

IDF_VERSION="${IDF_VERSION:-v6.0.2}"
IDF_CLONE_DIR="${IDF_CLONE_DIR:-$HOME/esp/esp-idf}"
IDF_TARGETS="${IDF_TARGETS:-esp32s3}"

command -v git >/dev/null 2>&1 || die "git is required."

# Platform-specific prerequisite hints (install.sh does not install these for you).
case "$(uname -s)" in
  Darwin)
    command -v cmake >/dev/null 2>&1 || warn "If the install fails, run: brew install cmake ninja dfu-util" ;;
  Linux)
    warn "Debian/Ubuntu prerequisites (once): sudo apt install -y git wget flex bison gperf \
python3 python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0" ;;
esac

# --- pick Python (ESP-IDF supports 3.10–3.14) ---
if [ -n "${IDF_PYTHON:-}" ] && command -v "$IDF_PYTHON" >/dev/null 2>&1; then
  PYTHON="$(command -v "$IDF_PYTHON")"
else
  PYTHON="$(command -v python3 || true)"
fi
[ -z "$PYTHON" ] && die "No python3 found. Install Python 3.10–3.14."

PYV="$("$PYTHON" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
case "$PYV" in
  3.1[0-4]) ok "Using Python $PYV ($PYTHON)" ;;
  *) warn "Python $PYV is outside ESP-IDF's supported range 3.10–3.14."
     warn "Install one in that range and re-run with IDF_PYTHON=python3.XX if the installer complains." ;;
esac

# Make sure the chosen Python can verify TLS. python.org's macOS Python ships without a
# linked CA bundle, which makes idf_tools downloads fail with CERTIFICATE_VERIFY_FAILED.
# If a live HTTPS probe fails, point urllib at certifi's bundle for this install only
# (scoped via SSL_CERT_FILE — no system changes; a no-op where certs already work).
probe_tls() { "$PYTHON" -c 'import urllib.request; urllib.request.urlopen("https://github.com", timeout=15)' >/dev/null 2>&1; }
if ! probe_tls; then
  CA="$("$PYTHON" -c 'import certifi; print(certifi.where())' 2>/dev/null || true)"
  if [ -n "$CA" ] && [ -f "$CA" ]; then
    export SSL_CERT_FILE="$CA"
    log "TLS: default CA bundle missing — using certifi ($CA)"
  fi
  if ! probe_tls; then
    warn "Python ($PYV) still can't verify TLS certificates. Fix it, then re-run:"
    warn "  macOS (python.org): open \"/Applications/Python $PYV/Install Certificates.command\""
    warn "  any OS:             \"$PYTHON\" -m pip install --upgrade certifi"
  else
    ok "TLS verification OK (via certifi)"
  fi
fi

# --- clone / update esp-idf ---
if [ -d "$IDF_CLONE_DIR/.git" ]; then
  log "Updating ESP-IDF at $IDF_CLONE_DIR → $IDF_VERSION"
  git -C "$IDF_CLONE_DIR" fetch --tags --depth 1 origin "$IDF_VERSION"
  git -C "$IDF_CLONE_DIR" checkout "$IDF_VERSION"
  git -C "$IDF_CLONE_DIR" submodule update --init --recursive --depth 1
else
  log "Cloning ESP-IDF $IDF_VERSION → $IDF_CLONE_DIR (this is a large download)"
  mkdir -p "$(dirname "$IDF_CLONE_DIR")"
  git clone --branch "$IDF_VERSION" --depth 1 --recursive \
    https://github.com/espressif/esp-idf.git "$IDF_CLONE_DIR"
fi

# --- run the installer with our chosen Python first on PATH ---
log "Installing tools for: $IDF_TARGETS (downloads the toolchain, several hundred MB)"
TMPBIN="$(mktemp -d)"
trap 'rm -rf "$TMPBIN"' EXIT
ln -sf "$PYTHON" "$TMPBIN/python3"
ln -sf "$PYTHON" "$TMPBIN/python"
PATH="$TMPBIN:$PATH" "$IDF_CLONE_DIR/install.sh" "$IDF_TARGETS"

# install.sh installs the cross-toolchain + Python env, but on macOS/Linux it does NOT
# install cmake/ninja (idf.py needs both). Pull them from Espressif's tool registry so the
# build is fully self-contained — no Homebrew/apt required. Idempotent.
log "Ensuring cmake and ninja are installed"
"$PYTHON" "$IDF_CLONE_DIR/tools/idf_tools.py" install cmake ninja

ok "ESP-IDF $IDF_VERSION installed at $IDF_CLONE_DIR"
cat <<EOF

Next steps:
  • The repo scripts (build-firmware.sh, flash.sh, dev-firmware.sh) auto-detect this install.
      ./scripts/dev-firmware.sh
  • To use idf.py directly in your shell:
      . "$IDF_CLONE_DIR/export.sh"
EOF
