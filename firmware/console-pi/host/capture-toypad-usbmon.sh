#!/bin/bash
# Capture USB traffic for LEGO Toy Pad (VID:PID 0e6f:0241) using usbmon+tshark.
#
# Usage:
#   sudo ./capture-toypad-usbmon.sh physical
#   sudo ./capture-toypad-usbmon.sh virtual 20
#
# Args:
#   $1 label     required, e.g. physical|virtual
#   $2 duration  optional seconds (default: 20)

set -euo pipefail

LABEL="${1:-}"
SETTLE="${2:-10}"

if [[ -z "${LABEL}" ]]; then
  echo "Usage: sudo $0 <label> [settle_seconds_after_connect]" >&2
  echo "  Example: sudo $0 physical 10" >&2
  exit 1
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0 ${LABEL} ${SETTLE}" >&2
  exit 1
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "ERROR: tcpdump is required. Install with: sudo apt install -y tcpdump" >&2
  exit 1
fi

OUT_DIR="$(cd "$(dirname "$0")" && pwd)/traces"
mkdir -p "${OUT_DIR}"

modprobe usbmon

# Determine which USB bus to capture on before the device is connected.
# If the device is already attached, use that bus; otherwise prompt and detect on plug-in.
BUSNUM=""
DEVNUM=""
IFACE=""

EXISTING="$(lsusb -d 0e6f:0241 | head -n1 || true)"
if [[ -n "${EXISTING}" ]]; then
  echo "WARNING: Toy Pad is already connected. For best results, unplug it first," >&2
  echo "         then re-run this script and plug in when prompted." >&2
  echo "Continuing with already-connected device in 3s..." >&2
  sleep 3
  BUS="$(awk '{print $2}' <<< "${EXISTING}")"
  DEV="$(awk '{print $4}' <<< "${EXISTING}" | tr -d ':')"
  BUSNUM=$((10#${BUS}))
  DEVNUM=$((10#${DEV}))
  IFACE="usbmon${BUSNUM}"   # 10# strips leading zeros so lsusb '001' -> usbmon1
else
  # Determine which bus to watch. Strip leading zeros: lsusb prints '001', kernel names it usbmon1.
  CANDIDATE_BUS="$(lsusb | awk '{print $2}' | sort -u | head -n1 || echo '1')"
  IFACE="usbmon$((10#${CANDIDATE_BUS}))"
  echo "No Toy Pad detected yet."
  echo "Capture will start on ${IFACE} — plug the Toy Pad in when you see the prompt below."
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
PREFIX="${OUT_DIR}/${LABEL}-${STAMP}"
PCAP="${PREFIX}.pcapng"
META="${PREFIX}.meta.txt"

echo ""
echo "====================================================="
echo " PLUG IN THE TOY PAD NOW"
echo " Capturing on ${IFACE} for ${SETTLE}s after connect."
echo " This records the full enumeration + init handshake."
echo "====================================================="
echo ""

# Use tcpdump for capture (works as root without dumpcap/wireshark group issues).
# tshark is only used for reading/analysis of the saved pcap.
tcpdump -i "${IFACE}" -w "${PCAP}" >/dev/null 2>&1 &
TCPDUMP_PID=$!

# If the device wasn't already plugged, wait for it to appear.
if [[ -z "${DEVNUM}" ]]; then
  echo -n "Waiting for 0e6f:0241..."
  WAITED=0
  while true; do
    FOUND="$(lsusb -d 0e6f:0241 | head -n1 || true)"
    if [[ -n "${FOUND}" ]]; then
      echo " connected."
      BUS="$(awk '{print $2}' <<< "${FOUND}")"
      DEV="$(awk '{print $4}' <<< "${FOUND}" | tr -d ':')"
      BUSNUM=$((10#${BUS}))
      DEVNUM=$((10#${DEV}))
      echo "  Device: bus=${BUSNUM} dev=${DEVNUM}"
      break
    fi
    sleep 0.2
    WAITED=$((WAITED + 1))
    if [[ ${WAITED} -gt 150 ]]; then
      echo ""
      echo "ERROR: No Toy Pad detected after 30s. Stopping capture." >&2
      kill -INT "${TCPDUMP_PID}" 2>/dev/null || true
      sleep 1
      kill -KILL "${TCPDUMP_PID}" 2>/dev/null || true
      exit 1
    fi
    echo -n "."
  done
fi

echo "Recording ${SETTLE}s of traffic (enumeration + init handshake + early comms)..."
sleep "${SETTLE}"

# Ask tcpdump to flush/close capture, then force-kill if it does not exit.
kill -INT "${TCPDUMP_PID}" 2>/dev/null || true
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if ! kill -0 "${TCPDUMP_PID}" 2>/dev/null; then
    break
  fi
  sleep 0.2
done
kill -KILL "${TCPDUMP_PID}" 2>/dev/null || true

# Do not wait here; some tcpdump/usbmon combinations can leave wait() hanging.
sleep 0.2

{
  echo "label=${LABEL}"
  echo "settle_seconds=${SETTLE}"
  echo "timestamp=${STAMP}"
  echo "bus=${BUSNUM}"
  echo "device=${DEVNUM}"
  echo "usbmon_interface=${IFACE}"
  echo "pcap=${PCAP}"
} > "${META}"

echo ""
echo "Saved: ${PCAP}"
echo "Meta : ${META}"
echo ""
echo "Summarize with:"
echo "  $(dirname "$0")/summarize-toypad-usbmon.sh ${META}"
