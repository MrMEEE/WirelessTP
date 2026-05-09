#!/bin/bash
# Capture a USB fingerprint for the currently attached Toy Pad-like device.
# Usage:
#   sudo ./capture-toypad-fingerprint.sh physical
#   sudo ./capture-toypad-fingerprint.sh virtual

set -euo pipefail

LABEL="${1:-}"
if [[ -z "${LABEL}" ]]; then
  echo "Usage: sudo $0 <label>" >&2
  exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0 ${LABEL}" >&2
  exit 1
fi

OUT_DIR="$(cd "$(dirname "$0")" && pwd)/fingerprints"
mkdir -p "${OUT_DIR}"
STAMP="$(date +%Y%m%d-%H%M%S)"
PREFIX="${OUT_DIR}/${LABEL}-${STAMP}"

DEV_LINE="$(lsusb -d 0e6f:0241 | head -n1 || true)"
if [[ -z "${DEV_LINE}" ]]; then
  echo "No 0e6f:0241 device found." >&2
  exit 1
fi

echo "${DEV_LINE}" | tee "${PREFIX}.summary.txt"

BUS="$(awk '{print $2}' <<< "${DEV_LINE}")"
DEV="$(awk '{print $4}' <<< "${DEV_LINE}" | tr -d ':')"
BUSNUM=$((10#${BUS}))
DEVNUM=$((10#${DEV}))

RAW_DEV="/dev/bus/usb/$(printf '%03d' "${BUSNUM}")/$(printf '%03d' "${DEVNUM}")"

lsusb -v -d 0e6f:0241 > "${PREFIX}.lsusb-v.txt" 2>&1 || true
lsusb -D "${RAW_DEV}" > "${PREFIX}.lsusb-D.txt" 2>&1 || true

# Kernel view for this physical port path
SYS_PATH="/sys/bus/usb/devices"
find "${SYS_PATH}" -maxdepth 1 -type l | while read -r p; do
  if [[ -f "${p}/idVendor" && -f "${p}/idProduct" ]]; then
    v="$(cat "${p}/idVendor" 2>/dev/null || true)"
    d="$(cat "${p}/idProduct" 2>/dev/null || true)"
    if [[ "${v}" == "0e6f" && "${d}" == "0241" ]]; then
      {
        echo "path=${p}"
        for f in busnum devnum bcdDevice bcdUSB bDeviceClass bDeviceSubClass bDeviceProtocol bMaxPacketSize0 manufacturer product serial speed version; do
          [[ -f "${p}/${f}" ]] && echo "${f}=$(cat "${p}/${f}")"
        done
      } > "${PREFIX}.sysfs.txt"
      break
    fi
  fi
done

echo "Saved fingerprint files with prefix: ${PREFIX}"
