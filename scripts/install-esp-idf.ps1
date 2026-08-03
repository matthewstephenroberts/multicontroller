<#
  install-esp-idf.ps1 — install ESP-IDF (framework + toolchain) on Windows (PowerShell).

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\install-esp-idf.ps1
    powershell -ExecutionPolicy Bypass -File scripts\install-esp-idf.ps1 -Version v6.0.2 -Targets esp32s3

  Requires git and Python 3.10–3.14 on PATH (https://git-scm.com, https://python.org).
  Tip: Espressif also ships a one-click Windows installer (ESP-IDF Tools Installer) if you
  prefer a GUI: https://dl.espressif.com/dl/esp-idf/
#>
param(
  [string]$Version  = $env:IDF_VERSION,
  [string]$CloneDir = $env:IDF_CLONE_DIR,
  [string]$Targets  = $env:IDF_TARGETS
)

$ErrorActionPreference = "Stop"
if (-not $Version)  { $Version  = "v6.0.2" }
if (-not $CloneDir) { $CloneDir = Join-Path $HOME "esp\esp-idf" }
if (-not $Targets)  { $Targets  = "esp32s3" }

function Info($m) { Write-Host "> $m" -ForegroundColor Blue }
function Ok($m)   { Write-Host "OK $m" -ForegroundColor Green }
function Die($m)  { Write-Host "x $m" -ForegroundColor Red; exit 1 }

if (-not (Get-Command git -ErrorAction SilentlyContinue))    { Die "git is required (https://git-scm.com)." }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Die "Python 3.10-3.14 is required (https://python.org)." }

$pyv = (python -c "import sys; print('%d.%d' % sys.version_info[:2])")
if ($pyv -notin @("3.10","3.11","3.12","3.13","3.14")) {
  Write-Host "! Python $pyv is outside ESP-IDF's supported range 3.10-3.14." -ForegroundColor Yellow
}

# Ensure Python can verify TLS; if not, point urllib at certifi's CA bundle for this install
# (scoped via SSL_CERT_FILE — no system changes; a no-op where certs already work).
function Test-Tls { python -c "import urllib.request; urllib.request.urlopen('https://github.com', timeout=15)" 2>$null; return $LASTEXITCODE -eq 0 }
if (-not (Test-Tls)) {
  $ca = (python -c "import certifi; print(certifi.where())" 2>$null)
  if ($ca -and (Test-Path $ca)) {
    $env:SSL_CERT_FILE = $ca
    Info "TLS: default CA bundle missing - using certifi ($ca)"
  }
  if (-not (Test-Tls)) {
    Write-Host "! Python cannot verify TLS certificates. Fix with:" -ForegroundColor Yellow
    Write-Host "    python -m pip install --upgrade certifi" -ForegroundColor Yellow
  } else { Ok "TLS verification OK (via certifi)" }
}

if (Test-Path (Join-Path $CloneDir ".git")) {
  Info "Updating ESP-IDF at $CloneDir -> $Version"
  git -C $CloneDir fetch --tags --depth 1 origin $Version
  git -C $CloneDir checkout $Version
  git -C $CloneDir submodule update --init --recursive --depth 1
} else {
  Info "Cloning ESP-IDF $Version -> $CloneDir (large download)"
  $parent = Split-Path $CloneDir
  if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
  git clone --branch $Version --depth 1 --recursive https://github.com/espressif/esp-idf.git $CloneDir
}

Info "Installing tools for: $Targets (downloads the toolchain, several hundred MB)"
& (Join-Path $CloneDir "install.ps1") $Targets

Ok "ESP-IDF $Version installed at $CloneDir"
Write-Host ""
Write-Host "Next steps (in a new PowerShell):"
Write-Host "    . `"$CloneDir\export.ps1`""
Write-Host "    idf.py -C firmware set-target esp32s3"
Write-Host "    idf.py -C firmware build flash monitor"
