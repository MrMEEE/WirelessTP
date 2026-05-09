#!/bin/bash
# Quick host diagnostics for Toy Pad + RPCS3 claim issues.

set -euo pipefail

echo "== usbhid quirks param =="
if [[ -r /sys/module/usbhid/parameters/quirks ]]; then
  cat /sys/module/usbhid/parameters/quirks
else
  echo "(missing: /sys/module/usbhid/parameters/quirks)"
fi

echo
echo "== loaded modules (hid/usbhid) =="
lsmod | grep -E '(^usbhid|^hid_generic|^hid )' || true

echo
echo "== Toy Pad USB nodes =="
lsusb -d 0e6f:0241 || true

echo
echo "== Kernel log (recent Toy Pad lines) =="
dmesg | tail -n 400 | grep -E '0e6f|0241|usbfs: interface 0|did not claim interface 0' || true
