#!/bin/bash
# Disable host-side Toy Pad mode for RPCS3 on Linux.

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

MODPROBE_FILE="/etc/modprobe.d/usbhid-toypad-rpcs3.conf"
UDEV_FILE="/etc/udev/rules.d/99-toypad-rpcs3.rules"

rm -f "${MODPROBE_FILE}" "${UDEV_FILE}"

udevadm control --reload-rules
udevadm trigger || true

if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u
elif command -v dracut >/dev/null 2>&1; then
  dracut -f
fi

echo
echo "RPCS3 Toy Pad host mode disabled."
echo "Reboot recommended to restore normal usbhid behavior:"
echo "  sudo reboot"
