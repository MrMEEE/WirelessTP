#!/bin/bash
# Compare two captured Toy Pad fingerprints.
# Usage:
#   ./diff-toypad-fingerprint.sh /path/a-prefix /path/b-prefix
# Example:
#   ./diff-toypad-fingerprint.sh fingerprints/physical-20260505-120000 fingerprints/virtual-20260505-120300

set -euo pipefail

A="${1:-}"
B="${2:-}"
if [[ -z "${A}" || -z "${B}" ]]; then
  echo "Usage: $0 <prefixA> <prefixB>" >&2
  exit 1
fi

for ext in summary.txt lsusb-v.txt lsusb-D.txt sysfs.txt; do
  AF="${A}.${ext}"
  BF="${B}.${ext}"
  echo "===== ${ext} ====="
  if [[ -f "${AF}" && -f "${BF}" ]]; then
    diff -u "${AF}" "${BF}" || true
  else
    echo "Missing files for ${ext}:"
    [[ ! -f "${AF}" ]] && echo "  missing ${AF}"
    [[ ! -f "${BF}" ]] && echo "  missing ${BF}"
  fi
  echo

done
