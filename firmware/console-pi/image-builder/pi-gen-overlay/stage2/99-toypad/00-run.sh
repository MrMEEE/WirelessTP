#!/bin/bash -e

install -d "${ROOTFS_DIR}/opt/toypad"
install -m 0755 "files/setup_gadget.sh" "${ROOTFS_DIR}/opt/toypad/setup_gadget.sh"
install -m 0755 "files/toy_daemon.py" "${ROOTFS_DIR}/opt/toypad/toy_daemon.py"
install -m 0644 "files/toypad.service" "${ROOTFS_DIR}/etc/systemd/system/toypad.service"

# Ensure gadget modules are loaded at boot.
install -d "${ROOTFS_DIR}/etc/modules-load.d"
cat > "${ROOTFS_DIR}/etc/modules-load.d/toypad.conf" <<'EOF'
dwc2
libcomposite
EOF

# Enable service at boot.
install -d "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants"
ln -sf "/etc/systemd/system/toypad.service" \
  "${ROOTFS_DIR}/etc/systemd/system/multi-user.target.wants/toypad.service"

# Ensure dwc2 overlay and UART are enabled in config.txt.
CONFIG_TXT="${ROOTFS_DIR}/boot/firmware/config.txt"
if [ ! -f "${CONFIG_TXT}" ]; then
  CONFIG_TXT="${ROOTFS_DIR}/boot/config.txt"
fi

CMDLINE_TXT="${ROOTFS_DIR}/boot/firmware/cmdline.txt"
if [ ! -f "${CMDLINE_TXT}" ]; then
  CMDLINE_TXT="${ROOTFS_DIR}/boot/cmdline.txt"
fi

if ! grep -q '^dtoverlay=dwc2$' "${CONFIG_TXT}"; then
  echo "dtoverlay=dwc2" >> "${CONFIG_TXT}"
fi

if ! grep -q '^enable_uart=1$' "${CONFIG_TXT}"; then
  echo "enable_uart=1" >> "${CONFIG_TXT}"
fi

if [ -f "${CMDLINE_TXT}" ] && ! grep -q 'modules-load=dwc2,libcomposite' "${CMDLINE_TXT}"; then
  # cmdline.txt must stay one line.
  sed -i '1 s#$# modules-load=dwc2,libcomposite#' "${CMDLINE_TXT}"
fi
