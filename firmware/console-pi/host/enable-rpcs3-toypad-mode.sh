#!/bin/bash
# Enable host-side Toy Pad mode for RPCS3 on Linux.
# This prevents usbhid from binding to VID:PID 0e6f:0241 and avoids
# interface ownership races seen as "did not claim interface 0 before use".

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

MODPROBE_FILE="/etc/modprobe.d/usbhid-toypad-rpcs3.conf"
UDEV_FILE="/etc/udev/rules.d/99-toypad-rpcs3.rules"

cat > "${MODPROBE_FILE}" <<'EOF'
# Ignore LEGO Dimensions Toy Pad for usbhid so RPCS3/libusb can claim it directly.
# 0x0004 = HID_QUIRK_IGNORE
options usbhid quirks=0x0e6f:0x0241:0x0004
EOF

cat > "${UDEV_FILE}" <<'EOF'
# Allow user-space USB access for the Toy Pad device node.
SUBSYSTEM=="usb", ATTR{idVendor}=="0e6f", ATTR{idProduct}=="0241", MODE="0666"
EOF

udevadm control --reload-rules
udevadm trigger || true

if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u
elif command -v dracut >/dev/null 2>&1; then
  dracut -f
fi

echo
echo "RPCS3 Toy Pad host mode enabled."
echo "Reboot required for the usbhid quirk to take full effect:"
echo "  sudo reboot"
