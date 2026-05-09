#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SO="${SCRIPT_DIR}/rpcs3_libusb_claim_shim.so"

run_with_shim() {
  local bin="$1"
  shift || true
  exec env LD_PRELOAD="${SO}${LD_PRELOAD:+:${LD_PRELOAD}}" "$bin" "$@"
}

if [[ ! -f "${SO}" ]]; then
  "${SCRIPT_DIR}/build-rpcs3-libusb-claim-shim.sh"
fi

if [[ $# -eq 0 ]]; then
  if [[ -n "${RPCS3_BIN:-}" && -x "${RPCS3_BIN}" ]]; then
    run_with_shim "${RPCS3_BIN}"
  fi

  if command -v rpcs3 >/dev/null 2>&1; then
    run_with_shim rpcs3
  fi

  if [[ -x /usr/bin/rpcs3 ]]; then
    run_with_shim /usr/bin/rpcs3
  fi

  # Common local AppImage naming patterns.
  shopt -s nullglob
  for candidate in "${SCRIPT_DIR}"/rpcs3*.AppImage "${SCRIPT_DIR}"/rpcs3*.appimage; do
    if [[ -f "${candidate}" ]]; then
      if [[ ! -x "${candidate}" ]]; then
        chmod +x "${candidate}" || true
      fi
      if [[ -x "${candidate}" ]]; then
        run_with_shim "${candidate}"
      fi
    fi
  done
  shopt -u nullglob

  # Fall back to first AppImage in current working directory.
  shopt -s nullglob
  for candidate in "$PWD"/rpcs3*.AppImage "$PWD"/rpcs3*.appimage; do
    if [[ -f "${candidate}" ]]; then
      if [[ ! -x "${candidate}" ]]; then
        chmod +x "${candidate}" || true
      fi
      if [[ -x "${candidate}" ]]; then
        run_with_shim "${candidate}"
      fi
    fi
  done
  shopt -u nullglob

  echo "Could not find an RPCS3 executable."
  echo "Tried: RPCS3_BIN, PATH (rpcs3), /usr/bin/rpcs3, local AppImage patterns."
  echo "Usage: $0 /path/to/rpcs3 [args...]"
  echo "Example: $0 ./rpcs3-v0.0.40-19296-e8cd6f4e_linux64.AppImage"
  exit 1
fi

if [[ ! -x "$1" ]]; then
  echo "RPCS3 binary is not executable: $1"
  echo "Run: chmod +x $1"
  exit 1
  fi

run_with_shim "$@"
