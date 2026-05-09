#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SCRIPT_DIR}/rpcs3_libusb_claim_shim.c"
OUT="${SCRIPT_DIR}/rpcs3_libusb_claim_shim.so"

gcc -shared -fPIC -O2 -Wall -Wextra -o "${OUT}" "${SRC}" -ldl -pthread

echo "Built ${OUT}"
