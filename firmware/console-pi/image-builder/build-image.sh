#!/bin/bash
# Build a preconfigured DietPi image for Pi Zero 1.3 (no wireless).
#
# The Pi Zero acts purely as a USB HID gadget toward the PC (dwc2/libcomposite)
# and as a UART bridge to the Console ESP32.  WiFi, web UI, and captive portal
# all live on the ESP32 — nothing of that is needed here.
#
# DietPi RPi1 ARMv6 Bookworm covers Pi Zero v1.2 and v1.3 (no wireless).
#
# Connections:
#   Pi Zero data micro-USB  →  PC (appears as LEGO Toy Pad HID device)
#   Pi Zero GPIO 14 (TX)    →  Console ESP32 RX
#   Pi Zero GPIO 15 (RX)    →  Console ESP32 TX
#   Pi Zero GND             →  Console ESP32 GND
#
# Usage:
#   ./build-image.sh                # full rebuild (download/extract/patch)
#   ./build-image.sh quick          # patch existing output image in place
#   HOSTNAME=wirelesstp ./build-image.sh
#   OUTPUT_DIR=/tmp ./build-image.sh quick
#
# Output: OUTPUT_DIR/WirelessTP-ToyPad.img  (ready to flash with Raspberry Pi Imager or dd)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-${SCRIPT_DIR}/output}"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_MODE="${BUILD_MODE:-full}"

if [[ $# -gt 0 ]]; then
  case "$1" in
    full|quick)
      BUILD_MODE="$1"
      shift
      ;;
    *)
      echo "ERROR: unknown mode '$1' (expected 'full' or 'quick')." >&2
      exit 1
      ;;
  esac
fi

DEVICE_HOSTNAME="${DEVICE_HOSTNAME:-wirelesstp-toypad}"
ROOT_PASS="${ROOT_PASS:-dietpi}"
TIMEZONE="${TIMEZONE:-Etc/UTC}"
# 1 = keep serial login console on ttyAMA0 (useful for ESP32 debug bridge)
# 0 = mask serial getty so ttyAMA0 can be dedicated to LP/UART bridge
ENABLE_UART_CONSOLE="${ENABLE_UART_CONSOLE:-0}"

# DietPi ARMv6 Bookworm — Pi Zero v1.2 / v1.3 (no wireless)
DIETPI_URL="https://dietpi.com/downloads/images/DietPi_RPi1-ARMv6-Bookworm.img.xz"
DIETPI_ARCHIVE="${CACHE_DIR}/DietPi_RPi1-ARMv6-Bookworm.img.xz"
OUTPUT_IMG="${OUTPUT_DIR}/WirelessTP-ToyPad.img"

# ─── Preflight ───────────────────────────────────────────────────────────────

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: '$1' is required but not installed." >&2
    echo "       Install with: sudo apt install -y ${2:-$1}" >&2
    exit 1
  }
}

require_cmd losetup util-linux
require_cmd mount util-linux

if [[ "${BUILD_MODE}" == "full" ]]; then
  require_cmd wget
  require_cmd xz   xz-utils
fi

if [[ ! -f "${PROJECT_ROOT}/firmware/console-pi/setup_gadget.sh" ]]; then
  echo "ERROR: missing firmware/console-pi/setup_gadget.sh" >&2; exit 1
fi
if [[ ! -f "${PROJECT_ROOT}/firmware/console-pi/toy_daemon.py" ]]; then
  echo "ERROR: missing firmware/console-pi/toy_daemon.py" >&2; exit 1
fi

mkdir -p "${OUTPUT_DIR}" "${CACHE_DIR}"

if [[ "${BUILD_MODE}" == "full" ]]; then
  # ─── Download ──────────────────────────────────────────────────────────────

  if [[ ! -f "${DIETPI_ARCHIVE}" ]]; then
    echo "[1/4] Downloading DietPi image..."
    wget -q --show-progress -O "${DIETPI_ARCHIVE}" "${DIETPI_URL}"
  else
    echo "[1/4] DietPi archive already cached."
  fi

  # ─── Extract ───────────────────────────────────────────────────────────────

  echo "[2/4] Extracting image..."
  EXTRACT_DIR="${CACHE_DIR}/extracted"
  mkdir -p "${EXTRACT_DIR}"
  RAW_IMG="${EXTRACT_DIR}/dietpi.img"
  xz -d -k -c "${DIETPI_ARCHIVE}" > "${RAW_IMG}"
  if [[ ! -s "${RAW_IMG}" ]]; then
    echo "ERROR: extraction produced an empty file" >&2
    exit 1
  fi
  cp "${RAW_IMG}" "${OUTPUT_IMG}"
  echo "  base image → ${OUTPUT_IMG}"
else
  echo "[1/3] Quick mode: reusing existing image"
  if [[ ! -f "${OUTPUT_IMG}" ]]; then
    echo "ERROR: quick mode requires an existing image at ${OUTPUT_IMG}" >&2
    echo "       Run a full build once first: ./build-image.sh" >&2
    exit 1
  fi
fi

# ─── Mount boot partition ─────────────────────────────────────────────────────

if [[ "${BUILD_MODE}" == "full" ]]; then
  echo "[3/4] Injecting ToyPad configuration..."
else
  echo "[2/3] Quick patching ToyPad files..."
fi
MOUNT_DIR="${CACHE_DIR}/mnt"
ROOTFS_DIR="${CACHE_DIR}/rootfs"
mkdir -p "${MOUNT_DIR}" "${ROOTFS_DIR}"

LOOP_DEV=$(sudo losetup --find --show --partscan "${OUTPUT_IMG}")
echo "  loop device: ${LOOP_DEV}"
BOOT_PART="${LOOP_DEV}p1"
ROOTFS_PART="${LOOP_DEV}p2"
sleep 1
sudo mount "${BOOT_PART}" "${MOUNT_DIR}"
sudo mount "${ROOTFS_PART}" "${ROOTFS_DIR}"

cleanup() {
  # Unmount chroot bind mounts first (in reverse order), if they were mounted
  for d in dev/pts dev sys proc; do
    sudo umount "${ROOTFS_DIR}/$d" 2>/dev/null || true
  done
  sudo umount "${ROOTFS_DIR}" 2>/dev/null || true
  sudo umount "${MOUNT_DIR}" 2>/dev/null || true
  sudo losetup -d "${LOOP_DEV}" 2>/dev/null || true
}
trap cleanup EXIT

# ─── Configure serial getty in rootfs (before first boot) ────────────────────
sudo mkdir -p "${ROOTFS_DIR}/etc/systemd/system"
if [[ "${ENABLE_UART_CONSOLE}" == "1" ]]; then
  echo "  Keeping serial-getty enabled on ttyAMA0 (debug console mode)."
  sudo rm -f "${ROOTFS_DIR}/etc/systemd/system/serial-getty@ttyAMA0.service"
  sudo rm -f "${ROOTFS_DIR}/etc/systemd/system/serial-getty@ttyS0.service"
  sudo mkdir -p "${ROOTFS_DIR}/etc/systemd/system/getty.target.wants"
  sudo ln -sf /lib/systemd/system/serial-getty@.service \
    "${ROOTFS_DIR}/etc/systemd/system/getty.target.wants/serial-getty@ttyAMA0.service"
else
  echo "  Masking serial-getty on ttyAMA0 and ttyS0 in rootfs..."
  sudo ln -sf /dev/null "${ROOTFS_DIR}/etc/systemd/system/serial-getty@ttyAMA0.service"
  sudo ln -sf /dev/null "${ROOTFS_DIR}/etc/systemd/system/serial-getty@ttyS0.service"
fi

if [[ "${BUILD_MODE}" == "full" ]]; then

# ─── Write dietpi.txt ─────────────────────────────────────────────────────────

sudo tee "${MOUNT_DIR}/dietpi.txt" > /dev/null <<EOF
##### DietPi automation config — generated by WirelessTP build-image.sh #####

AUTO_SETUP_LOCALE=en_US.UTF-8
AUTO_SETUP_KEYBOARD_LAYOUT=us
AUTO_SETUP_TIMEZONE=${TIMEZONE}

AUTO_SETUP_NET_WIFI_ENABLED=0
AUTO_SETUP_NET_ETH_ENABLED=0
AUTO_SETUP_DHCP_TO_STATIC=0
AUTO_SETUP_NET_USERSDATA_LOAD=0

AUTO_SETUP_HEADLESS=1
AUTO_SETUP_AUTOMATED=1

# Disable DietPi update/APT checks — Pi Zero has no network.
CONFIG_CHECK_DIETPI_UPDATES=0
CONFIG_CHECK_APT_UPDATES=0

AUTO_SETUP_SSH_SERVER_INDEX=-2

AUTO_SETUP_GLOBAL_PASSWORD=${ROOT_PASS}

SURVEY_OPTED_IN=0

SOFTWARE_DISABLE_SSH_PASSWORD_LOGINS=0
EOF

# ─── Skip DietPi first-run update (the real fix) ─────────────────────────────
# DietPi's dietpi-update unconditionally pings 9.9.9.9 regardless of
# CONFIG_CHECK_DIETPI_UPDATES.  Install stage 10 means "pre-installed image":
# DietPi converts it to stage 2 (completed) on first boot and skips the entire
# firstrun-setup + update flow, so no network ping and no blocking TUI.
# Source: /boot/dietpi/func/dietpi-globals, G_DIETPI_INSTALL_STAGE comments.
echo '  Pre-setting DietPi install stage to 10 (skip firstrun update)...'
sudo mkdir -p "${MOUNT_DIR}/dietpi"
echo '10' | sudo tee "${MOUNT_DIR}/dietpi/.install_stage" > /dev/null

# ─── Download python3-serial .deb for offline install on Pi ─────────────────
# Pi Zero has no internet on first boot, so we bundle the .deb.  python3-serial
# is architecture-independent (_all.deb) and only depends on python3.
# Use 'apt-get download' so we always get the version matching the host apt cache
# (the _all.deb is identical between Ubuntu/Debian for this package).

PYSERIAL_DEB_GLOB="${CACHE_DIR}/python3-serial_*_all.deb"
PYSERIAL_DEB_DEST="${CACHE_DIR}/python3-serial_all.deb"
if [[ ! -s "${PYSERIAL_DEB_DEST}" ]]; then
  echo "  Downloading python3-serial.deb via apt-get download..."
  (cd "${CACHE_DIR}" && apt-get download python3-serial 2>&1)
  # apt-get download creates a versioned filename; rename to a fixed name for the script
  DEB_FILE=$(ls ${PYSERIAL_DEB_GLOB} 2>/dev/null | head -1)
  if [[ -z "${DEB_FILE}" || ! -s "${DEB_FILE}" ]]; then
    echo "ERROR: apt-get download python3-serial failed" >&2; exit 1
  fi
  mv "${DEB_FILE}" "${PYSERIAL_DEB_DEST}"
  echo "  python3-serial.deb cached ($(du -h "${PYSERIAL_DEB_DEST}" | cut -f1))"
else
  echo "  python3-serial.deb already cached."
fi

# ─── Install daemon files into rootfs at build time ─────────────────────────
# Everything that Automation_Custom_Script.sh used to do on first boot is now
# done here against the mounted rootfs so no first-boot internet access is
# needed at all.

# 1. Daemon files
echo '  Installing ToyPad daemon files into rootfs...'
sudo mkdir -p "${ROOTFS_DIR}/opt/toypad"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/setup_gadget.sh" "${ROOTFS_DIR}/opt/toypad/setup_gadget.sh"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/toy_daemon.py"   "${ROOTFS_DIR}/opt/toypad/toy_daemon.py"
sudo chmod +x "${ROOTFS_DIR}/opt/toypad/setup_gadget.sh" "${ROOTFS_DIR}/opt/toypad/toy_daemon.py"

# 2. python3-serial — extract the all-arch .deb directly into rootfs (no chroot needed)
echo '  Extracting python3-serial.deb into rootfs...'
sudo dpkg-deb --extract "${PYSERIAL_DEB_DEST}" "${ROOTFS_DIR}"

# 3. python3 — install into rootfs via chroot + qemu-arm{,-static}.
# DietPi's minimal image does not include python3 and dpkg --extract won't
# run update-alternatives post-install scripts.  A chroot install is the
# cleanest approach: it resolves deps and sets up the /usr/bin/python3 symlink.
echo '  Installing python3 into rootfs via chroot (uses build-host internet)...'
QEMU_ARM_BIN=''
if command -v qemu-arm-static >/dev/null 2>&1; then
  QEMU_ARM_BIN='qemu-arm-static'
elif command -v qemu-arm >/dev/null 2>&1; then
  QEMU_ARM_BIN='qemu-arm'
else
  echo "ERROR: neither 'qemu-arm-static' nor 'qemu-arm' is installed." >&2
  echo "       Install with: sudo apt install -y qemu-user-static qemu-user" >&2
  exit 1
fi

# Register ARM binfmt handler so chroot can run ARM binaries transparently
sudo update-binfmts --enable qemu-arm 2>/dev/null || true

# Copy qemu binary into rootfs
sudo cp "/usr/bin/${QEMU_ARM_BIN}" "${ROOTFS_DIR}/usr/bin/"

# Bind-mount pseudo-filesystems needed by apt/dpkg inside chroot
for d in proc sys dev dev/pts; do
  sudo mount --bind "/$d" "${ROOTFS_DIR}/$d"
done

# Use build-host DNS temporarily so apt-get can resolve mirrors
sudo cp "${ROOTFS_DIR}/etc/resolv.conf" "${ROOTFS_DIR}/etc/resolv.conf.bak" 2>/dev/null || true
sudo cp /etc/resolv.conf "${ROOTFS_DIR}/etc/resolv.conf"

# Ensure apt runtime directories exist inside rootfs.
# Some minimal images may miss one or more of these paths.
sudo mkdir -p "${ROOTFS_DIR}/var/cache/apt/archives/partial"
sudo mkdir -p "${ROOTFS_DIR}/var/lib/apt/lists/partial"
sudo chmod 755 "${ROOTFS_DIR}/var/cache/apt/archives" "${ROOTFS_DIR}/var/cache/apt/archives/partial" \
              "${ROOTFS_DIR}/var/lib/apt/lists" "${ROOTFS_DIR}/var/lib/apt/lists/partial" 2>/dev/null || true

# Run apt inside the rootfs
sudo chroot "${ROOTFS_DIR}" /bin/sh -c \
  'apt-get update -qq && apt-get install -y --no-install-recommends python3'

# Restore resolv.conf and tear down bind mounts
sudo mv "${ROOTFS_DIR}/etc/resolv.conf.bak" "${ROOTFS_DIR}/etc/resolv.conf" 2>/dev/null \
  || sudo rm -f "${ROOTFS_DIR}/etc/resolv.conf"
for d in dev/pts dev sys proc; do
  sudo umount "${ROOTFS_DIR}/$d" 2>/dev/null || true
done
sudo rm -f "${ROOTFS_DIR}/usr/bin/${QEMU_ARM_BIN}"

# 4. toypad.service — install and enable
echo '  Installing and enabling toypad.service...'
sudo cp "${SCRIPT_DIR}/pi-gen-overlay/stage2/99-toypad/files/toypad.service" \
        "${ROOTFS_DIR}/etc/systemd/system/toypad.service"
sudo mkdir -p "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/toypad.service \
     "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants/toypad.service"

# 5. Kernel modules to load at boot
sudo mkdir -p "${ROOTFS_DIR}/etc/modules-load.d"
printf 'dwc2\nlibcomposite\n' | sudo tee "${ROOTFS_DIR}/etc/modules-load.d/toypad.conf" > /dev/null

# 6. Hostname
echo '  Setting hostname...'
echo "${DEVICE_HOSTNAME}" | sudo tee "${ROOTFS_DIR}/etc/hostname" > /dev/null
# Update /etc/hosts — replace any 127.0.1.1 line, or append if absent
if sudo grep -q '127\.0\.1\.1' "${ROOTFS_DIR}/etc/hosts" 2>/dev/null; then
  sudo sed -i "s/^127\.0\.1\.1.*/127.0.1.1\t${DEVICE_HOSTNAME}/" "${ROOTFS_DIR}/etc/hosts"
else
  printf '127.0.1.1\t%s\n' "${DEVICE_HOSTNAME}" | sudo tee -a "${ROOTFS_DIR}/etc/hosts" > /dev/null
fi

# ─── Boot partition: hardware config ────────────────────────────────────────
# Done here instead of Automation_Custom_Script.sh so everything is ready
# before first boot without any runtime script.

echo '  Patching config.txt and cmdline.txt...'

# dwc2 USB gadget overlay
sudo grep -qxF 'dtoverlay=dwc2' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'dtoverlay=dwc2' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

# Hardware UART on GPIO 14/15 for LP link to Console ESP32
sudo grep -qxF 'enable_uart=1' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'enable_uart=1' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

# Disable Bluetooth so the hardware UART (ttyAMA0) is free for LP link
sudo grep -qxF 'dtoverlay=disable-bt' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'dtoverlay=disable-bt' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

# Remove serial console from cmdline.txt so kernel boot messages don't spam
# the Console ESP32 UART bridge (which causes bad-frame errors on the ESP32).
if sudo grep -q 'console=serial0\|console=ttyAMA0' "${MOUNT_DIR}/cmdline.txt" 2>/dev/null; then
  sudo sed -i -E 's/(^| )console=serial0,[0-9]+( |$)/ /g; s/(^| )console=ttyAMA0,[0-9]+( |$)/ /g; s/  +/ /g; s/^ //; s/ $//' \
    "${MOUNT_DIR}/cmdline.txt"
fi

else

# ─── Quick mode: override only runtime files ─────────────────────────────────
echo '  Quick mode: replacing /opt/toypad files and service only...'
sudo mkdir -p "${ROOTFS_DIR}/opt/toypad"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/setup_gadget.sh" "${ROOTFS_DIR}/opt/toypad/setup_gadget.sh"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/toy_daemon.py"   "${ROOTFS_DIR}/opt/toypad/toy_daemon.py"
sudo chmod +x "${ROOTFS_DIR}/opt/toypad/setup_gadget.sh" "${ROOTFS_DIR}/opt/toypad/toy_daemon.py"

sudo cp "${SCRIPT_DIR}/pi-gen-overlay/stage2/99-toypad/files/toypad.service" \
        "${ROOTFS_DIR}/etc/systemd/system/toypad.service"
sudo mkdir -p "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/toypad.service \
     "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants/toypad.service"

# Keep current scripts mirrored on /boot for diagnostics.
sudo mkdir -p "${MOUNT_DIR}/wirelesstp"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/setup_gadget.sh" "${MOUNT_DIR}/wirelesstp/setup_gadget.sh"
sudo cp "${PROJECT_ROOT}/firmware/console-pi/toy_daemon.py"   "${MOUNT_DIR}/wirelesstp/toy_daemon.py"

# Also enforce boot-side gadget settings in quick mode so stale images
# cannot lose dwc2/libcomposite configuration.
echo '  Quick mode: patching config.txt and cmdline.txt...'
sudo grep -qxF 'dtoverlay=dwc2' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'dtoverlay=dwc2' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

sudo grep -qxF 'enable_uart=1' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'enable_uart=1' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

sudo grep -qxF 'dtoverlay=disable-bt' "${MOUNT_DIR}/config.txt" 2>/dev/null \
  || echo 'dtoverlay=disable-bt' | sudo tee -a "${MOUNT_DIR}/config.txt" > /dev/null

if ! sudo grep -q 'modules-load=dwc2,libcomposite' "${MOUNT_DIR}/cmdline.txt" 2>/dev/null; then
  sudo sed -i '1 s#$# modules-load=dwc2,libcomposite#' "${MOUNT_DIR}/cmdline.txt"
fi

if sudo grep -q 'console=serial0\|console=ttyAMA0' "${MOUNT_DIR}/cmdline.txt" 2>/dev/null; then
  sudo sed -i -E 's/(^| )console=serial0,[0-9]+( |$)/ /g; s/(^| )console=ttyAMA0,[0-9]+( |$)/ /g; s/  +/ /g; s/^ //; s/ $//' \
    "${MOUNT_DIR}/cmdline.txt"
fi

fi

if [[ "${BUILD_MODE}" == "full" ]]; then
  # Keep a copy of the python3-serial .deb on /boot for diagnostics/recovery.
  sudo mkdir -p "${MOUNT_DIR}/wirelesstp"
  sudo cp "${PYSERIAL_DEB_DEST}" "${MOUNT_DIR}/wirelesstp/python3-serial.deb"
else
  # In quick mode, copy the cached .deb only if present.
  if [[ -f "${CACHE_DIR}/python3-serial_all.deb" ]]; then
    sudo mkdir -p "${MOUNT_DIR}/wirelesstp"
    sudo cp "${CACHE_DIR}/python3-serial_all.deb" "${MOUNT_DIR}/wirelesstp/python3-serial.deb"
  fi
fi

# NOTE: Automation_Custom_Script.sh is intentionally absent.
# Everything that would have run there is now done above against the mounted
# rootfs/boot partitions, so no first-boot internet access or DietPi firstrun
# is required.

# ─── Done ────────────────────────────────────────────────────────────────────

sudo umount "${ROOTFS_DIR}"
sudo umount "${MOUNT_DIR}"
sudo losetup -d "${LOOP_DEV}"
trap - EXIT

echo ""
if [[ "${BUILD_MODE}" == "full" ]]; then
  echo "[4/4] Done."
else
  echo "[3/3] Done."
fi
echo ""
echo "  Output image : ${OUTPUT_IMG}"
echo "  Flash command: sudo dd if=${OUTPUT_IMG} of=/dev/sdX bs=4M status=progress conv=fsync"
echo "  Or use Raspberry Pi Imager → 'Use custom image'."
echo ""
if [[ "${BUILD_MODE}" == "full" ]]; then
  echo "  First boot is fast — DietPi firstrun is skipped (pre-installed image mode)."
else
  echo "  Quick mode applied: existing image was patched in place."
fi
echo "  On first boot:"
echo "    USB:  data micro-USB → PC  (appears as LEGO Toy Pad HID device)"
echo "    UART: GPIO 14 (TX) / GPIO 15 (RX) → Console ESP32"
echo "    SSH:  requires Ethernet adapter or USB serial"
echo "    Root password: ${ROOT_PASS}"
