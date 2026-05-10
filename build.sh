#!/usr/bin/env bash
# build.sh — build all WirelessTP firmware targets.
# Usage:
#   ./build.sh              # build all targets
#   ./build.sh console-rp2040  # build one target by directory name
#   ./build.sh console-rp2040-ps  # build only RP2040 PlayStation profile
#   ./build.sh console-rp2040-xbox  # build only RP2040 Xbox 360 profile
#   ./build.sh --upload     # build + upload to connected device (all targets)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
UPLOAD=false
FILTER=""

for arg in "$@"; do
  case "$arg" in
    --upload|-u) UPLOAD=true ;;
    --help|-h)
      echo "Usage: $0 [target-dir] [--upload]"
      echo "  target-dir  one of: console-rp2040, console-rp2040-ps, console-rp2040-xbox, console-esp32, pad-esp32, pad-esp32-nousb"
      exit 0
      ;;
    *) FILTER="$arg" ;;
  esac
done

# ── colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

pass() { echo -e "${GREEN}${BOLD}✓ $*${RESET}"; }
fail() { echo -e "${RED}${BOLD}✗ $*${RESET}"; }
info() { echo -e "${CYAN}» $*${RESET}"; }

# ── firmware targets ─────────────────────────────────────────────────────────
# Each entry: "firmware-dir:env-name"
TARGETS=(
  "console-rp2040:console_rp2040_ps"
  "console-rp2040:console_rp2040_xbox360"
  "console-esp32:console_esp32"
  "pad-esp32:pad_esp32"
  "pad-esp32:pad_esp32_nousb"
)

FAILED=()
PASSED=()

# Resolve PlatformIO executable — check PATH first, then the default venv location.
PIO_BIN="pio"
if ! command -v pio &>/dev/null; then
  if [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    PIO_BIN="$HOME/.platformio/penv/bin/pio"
  else
    echo -e "${RED}Error: 'pio' not found. Install PlatformIO: https://docs.platformio.org/en/latest/core/installation/index.html${RESET}"
    exit 1
  fi
fi

build_target() {
  local fw_dir="$1"
  local env_name="$2"
  local fw_path="$REPO_ROOT/firmware/$fw_dir"

  info "Building [$env_name] in firmware/$fw_dir ..."
  if $UPLOAD; then
    "$PIO_BIN" run -d "$fw_path" -e "$env_name" --target upload
  else
    "$PIO_BIN" run -d "$fw_path" -e "$env_name"
  fi
}

echo ""
echo -e "${BOLD}=== WirelessTP firmware build ===${RESET}"
echo ""

for entry in "${TARGETS[@]}"; do
  fw_dir="${entry%%:*}"
  env_name="${entry##*:}"

  env_alias=""
  case "$env_name" in
    console_rp2040_ps) env_alias="console-rp2040-ps" ;;
    console_rp2040_xbox360) env_alias="console-rp2040-xbox" ;;
  esac

  # Apply optional target filter (directory, env name, or env alias)
  if [[ -n "$FILTER" && "$fw_dir" != "$FILTER" && "$env_name" != "$FILTER" && "$env_alias" != "$FILTER" ]]; then
    continue
  fi

  if build_target "$fw_dir" "$env_name"; then
    pass "$env_name"
    PASSED+=("$env_name")
  else
    fail "$env_name"
    FAILED+=("$env_name")
  fi
done

echo ""
echo -e "${BOLD}=== Results ===${RESET}"
echo "  Passed : ${#PASSED[@]}"
echo "  Failed : ${#FAILED[@]}"
if [[ ${#FAILED[@]} -gt 0 ]]; then
  echo ""
  for f in "${FAILED[@]}"; do
    fail "$f"
  done
  exit 1
fi

pass "All targets built successfully."
