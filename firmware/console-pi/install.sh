#!/bin/bash
# install.sh — One-shot setup for Raspberry Pi 3B / Zero 2W
# Run this once after flashing a fresh Raspberry Pi OS image.
# The Pi must have internet access.
#
# What it does:
#   1. Enables dwc2 USB OTG overlay in /boot/firmware/config.txt
#   2. Installs python3-serial (needed only if using --uart LP bridge)
#   3. Installs this directory to /opt/toypad/
#   4. Installs a systemd service that runs setup_gadget.sh + toy_daemon.py at boot

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="/opt/toypad"
CONFIG_FILE="/boot/firmware/config.txt"
CMDLINE_FILE="/boot/firmware/cmdline.txt"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

# --- 1. Enable dwc2 overlay ---
if [ ! -f "$CONFIG_FILE" ]; then
    # Older Raspberry Pi OS path
    CONFIG_FILE="/boot/config.txt"
fi

if [ ! -f "$CMDLINE_FILE" ]; then
    CMDLINE_FILE="/boot/cmdline.txt"
fi

if grep -q "dtoverlay=dwc2" "$CONFIG_FILE" 2>/dev/null; then
    echo "[1/4] dwc2 overlay already present in $CONFIG_FILE"
else
    echo "[1/4] Adding dtoverlay=dwc2 to $CONFIG_FILE"
    echo "" >> "$CONFIG_FILE"
    echo "# USB gadget mode" >> "$CONFIG_FILE"
    echo "dtoverlay=dwc2" >> "$CONFIG_FILE"
    echo "      => reboot required before continuing"
fi

if [ -f "$CMDLINE_FILE" ]; then
    if grep -q 'modules-load=dwc2,libcomposite' "$CMDLINE_FILE" 2>/dev/null; then
        echo "[1/4] cmdline already has modules-load=dwc2,libcomposite"
    else
        echo "[1/4] Adding modules-load=dwc2,libcomposite to $CMDLINE_FILE"
        # cmdline.txt must remain a single line.
        sed -i '1 s#$# modules-load=dwc2,libcomposite#' "$CMDLINE_FILE"
        echo "      => reboot required before continuing"
    fi
fi

# --- 2. Install python3-serial ---
echo "[2/4] Installing python3-serial..."
apt-get install -y python3-serial > /dev/null

# --- 3. Copy files ---
echo "[3/4] Installing to $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
cp "$SCRIPT_DIR/setup_gadget.sh" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/toy_daemon.py"   "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/setup_gadget.sh"
chmod +x "$INSTALL_DIR/toy_daemon.py"

# --- 4. systemd service ---
echo "[4/4] Installing systemd service"
cat > /etc/systemd/system/toypad.service << 'EOF'
[Unit]
Description=LEGO Dimensions Toy Pad USB Gadget
After=network.target

[Service]
Type=simple
ExecStartPre=/opt/toypad/setup_gadget.sh setup
ExecStart=/usr/bin/python3 /opt/toypad/toy_daemon.py
ExecStopPost=/opt/toypad/setup_gadget.sh teardown
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable toypad.service

echo ""
echo "Done. If dwc2 was just added, reboot now:"
echo "  sudo reboot"
echo ""
echo "After reboot, start manually for first test:"
echo "  sudo /opt/toypad/setup_gadget.sh"
echo "  sudo python3 /opt/toypad/toy_daemon.py"
echo ""
echo "Or start the systemd service:"
echo "  sudo systemctl start toypad"
