#!/bin/bash
# Summarize a Toy Pad usbmon capture produced by capture-toypad-usbmon.sh.
#
# Usage:
#   ./summarize-toypad-usbmon.sh /path/to/trace.meta.txt

set -euo pipefail

META_FILE="${1:-}"
if [[ -z "${META_FILE}" ]]; then
  echo "Usage: $0 <meta_file>" >&2
  exit 1
fi

if [[ ! -f "${META_FILE}" ]]; then
  echo "ERROR: meta file not found: ${META_FILE}" >&2
  exit 1
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "ERROR: tcpdump is required. Install with: sudo apt install -y tcpdump" >&2
  exit 1
fi

PCAP="$(grep '^pcap=' "${META_FILE}" | sed 's/^pcap=//')"
DEVNUM="$(grep '^device=' "${META_FILE}" | sed 's/^device=//')"

if [[ -z "${PCAP}" || ! -f "${PCAP}" ]]; then
  echo "ERROR: pcap file missing or not found (from ${META_FILE})" >&2
  exit 1
fi

if [[ -z "${DEVNUM}" ]]; then
  echo "ERROR: device number missing in meta file" >&2
  exit 1
fi

DEVPAD="$(printf "%03d" "${DEVNUM}")"

TMP_ALL="$(mktemp)"
TMP_DEV="$(mktemp)"
trap 'rm -f "${TMP_ALL}" "${TMP_DEV}"' EXIT

TCPDUMP_CMD=(tcpdump -nn -tttt -r "${PCAP}")

# In this environment packet reads may require elevated privileges.
if ! "${TCPDUMP_CMD[@]}" -c 1 >/dev/null 2>&1; then
  TCPDUMP_CMD=(sudo tcpdump -nn -tttt -r "${PCAP}")
fi

"${TCPDUMP_CMD[@]}" >"${TMP_ALL}" 2>/dev/null || true

if [[ ! -s "${TMP_ALL}" ]]; then
  echo "ERROR: failed to read ${PCAP}. Try: sudo $0 ${META_FILE}" >&2
  exit 1
fi

# usbmon/tcpdump formats vary by kernel/tcpdump version.
# Match both non-padded (1:59:0) and padded (1:059:0) device IDs.
awk -v dev="${DEVNUM}" -v devpad="${DEVPAD}" '
  index($0, ":" dev ":") || index($0, ":" devpad ":") || $0 ~ ("dev[[:space:]]+" dev "\\b")
' "${TMP_ALL}" >"${TMP_DEV}" || true

echo "=== Trace Summary ==="
echo "meta: ${META_FILE}"
echo "pcap: ${PCAP}"
echo "device address: ${DEVNUM}"
echo "reader: ${TCPDUMP_CMD[*]}"
echo "all packets in capture: $(wc -l < "${TMP_ALL}")"
echo "device-matched packets: $(wc -l < "${TMP_DEV}")"
echo

echo "=== Packet Counts By Endpoint (device filtered) ==="
awk '
  {
    if (match($0, /[A-Za-z][A-Za-z]?:[0-9]+:[0-9]{3}:([0-9]+)/, m)) {
      ep = m[1]
      cnt[ep]++
    } else if (match($0, /\bep[[:space:]]*([0-9]+)\b/, m)) {
      ep = m[1]
      cnt[ep]++
    }
  }
  END {
    for (e in cnt) printf "%8d ep%s\n", cnt[e], e
  }
' "${TMP_DEV}" | sort -nr || true

echo
echo "=== First 120 Device Packets (endpoint, transfer, length) ==="
head -n 120 "${TMP_DEV}" || true

echo
echo "=== USB Enumeration / Control Transfers (SETUP stage) ==="
grep -Ei 'control|setup|get[ _-]?descriptor|set[ _-]?configuration|set[ _-]?interface|descriptor' "${TMP_DEV}" \
  | head -n 80 || true

echo
echo "=== HID Payload Candidates (first 120) ==="
grep -Ei 'interrupt|hid|\bep[[:space:]]*1\b|\bep[[:space:]]*129\b' "${TMP_DEV}" \
  | head -n 120 || true

if [[ ! -s "${TMP_DEV}" ]]; then
  echo
  echo "NOTE: no packets matched device ${DEVNUM} using current parser patterns."
  echo "Showing first 80 lines of raw capture for manual inspection:"
  head -n 80 "${TMP_ALL}" || true
fi
