#!/bin/bash
# USB HID gadget setup for Raspberry Pi Zero
# Emulates LEGO Dimensions Toy Pad: VID=0x0E6F PID=0x0241
# Based on the LD-ToyPad-Emulator usb_setup_script.sh by ags131 / Berny23
#
# Usage:
#   sudo ./setup_gadget.sh          # setup (default)
#   sudo ./setup_gadget.sh teardown # remove gadget

set -e

GADGET_NAME="g1"
GADGET_PATH="/sys/kernel/config/usb_gadget/$GADGET_NAME"

print_udc_diagnostics() {
    local cfg=""
    echo "Diagnostic info:" >&2
    echo "  kernel: $(uname -r)" >&2
    echo "  /sys/class/udc contents: $(ls /sys/class/udc 2>/dev/null || echo '<empty>')" >&2

    if [ -f /boot/firmware/config.txt ]; then
        cfg="/boot/firmware/config.txt"
    elif [ -f /boot/config.txt ]; then
        cfg="/boot/config.txt"
    fi

    if [ -n "$cfg" ]; then
        echo "  config file: $cfg" >&2
        grep -E '^(dtoverlay|otg_mode|dr_mode)=' "$cfg" >&2 || true
    else
        echo "  config file: not found (/boot/firmware/config.txt or /boot/config.txt)" >&2
    fi

    if [ -f /boot/firmware/cmdline.txt ]; then
        echo "  cmdline: $(cat /boot/firmware/cmdline.txt)" >&2
    elif [ -f /boot/cmdline.txt ]; then
        echo "  cmdline: $(cat /boot/cmdline.txt)" >&2
    fi

    echo "  loaded modules: $(lsmod | awk '{print $1}' | grep -E '^(dwc2|libcomposite)$' | xargs echo || echo '<none>')" >&2
}

write_if_exists() {
    local path="$1"
    local value="$2"
    if [ -f "$path" ]; then
        echo "$value" > "$path"
    fi
}

setup() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: must run as root (sudo)" >&2
        exit 1
    fi

    # Ensure OTG UDC + gadget framework are loaded.
    modprobe dwc2 || true
    modprobe libcomposite

    if [ -d "$GADGET_PATH" ]; then
        echo "Gadget '$GADGET_NAME' already exists; tearing down first."
        teardown
    fi

    echo "Setting up USB gadget: $GADGET_NAME"

    mkdir -p "$GADGET_PATH"
    cd "$GADGET_PATH"

    # Device descriptor
    # Force full-speed profile to better match real Toy Pad timing behavior.
    echo "full-speed" > max_speed
    echo "0x0200" > bcdUSB
    echo "0x0100" > bcdDevice
    echo "0x40"   > bMaxPacketSize0
    echo "0x0E6F" > idVendor
    echo "0x0241" > idProduct

    # String descriptors (English)
    mkdir -p strings/0x409
    echo "P.D.P.000000"      > strings/0x409/serialnumber
    echo "PDP LIMITED. "     > strings/0x409/manufacturer
    echo "LEGO READER V2.10" > strings/0x409/product

    # HID function
    mkdir -p functions/hid.g0
    # Keep Toy Pad HID profile values when the kernel exposes these attributes.
    write_if_exists functions/hid.g0/subclass 0
    write_if_exists functions/hid.g0/protocol 0
    write_if_exists functions/hid.g0/interval 1
    write_if_exists functions/hid.g0/poll_interval 1
    write_if_exists functions/hid.g0/in_interval 1
    write_if_exists functions/hid.g0/out_interval 1
    write_if_exists functions/hid.g0/bcdHID 0x0100
    write_if_exists functions/hid.g0/hid_version 0x0100
    echo 32 > functions/hid.g0/report_length
    printf "\x06\x00\xFF\x09\x01\xA1\x01\x19\x01\x29\x20\x15\x00\x26\xFF\x00\x75\x08\x95\x20\x81\x00\x19\x01\x29\x20\x91\x00\xC0" \
        > functions/hid.g0/report_desc

    # Configuration descriptor
    mkdir -p configs/c.1
    # Match physical Toy Pad: no iConfiguration string.
    [ -f configs/c.1/bmAttributes ] && echo 0x80 > configs/c.1/bmAttributes
    [ -f configs/c.1/MaxPower ] && echo 500 > configs/c.1/MaxPower

    # Link function into configuration
    ln -s functions/hid.g0 configs/c.1/

    # Activate: bind to the OTG UDC
    UDC=$(ls /sys/class/udc/ | head -1)
    if [ -z "$UDC" ]; then
        echo "ERROR: no UDC found in /sys/class/udc/ — is dtoverlay=dwc2 in config.txt?" >&2
        print_udc_diagnostics
        echo "Hint: add dtoverlay=dwc2 to config.txt and modules-load=dwc2,libcomposite to cmdline.txt, then reboot." >&2
        exit 1
    fi
    echo "Binding to UDC: $UDC"
    echo "$UDC" > UDC
    sleep 2
    chmod a+rw /dev/hidg0 || true

    echo "Gadget active. HID device: /dev/hidg0"
}

teardown() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: must run as root (sudo)" >&2
        exit 1
    fi

    if [ ! -d "$GADGET_PATH" ]; then
        echo "Gadget '$GADGET_NAME' not found; nothing to do."
        return
    fi

    echo "Removing gadget: $GADGET_NAME"
    cd "$GADGET_PATH"

    # Unbind from UDC
    echo "" > UDC 2>/dev/null || true

    # Remove symlinks
    rm -f configs/c.1/hid.g0 2>/dev/null || true

    # Remove subdirectories (must be done leaf-first)
    rmdir configs/c.1/strings/0x409 2>/dev/null || true
    rmdir configs/c.1               2>/dev/null || true
    rmdir functions/hid.g0          2>/dev/null || true
    rmdir strings/0x409             2>/dev/null || true
    cd /
    rmdir "$GADGET_PATH"

    echo "Gadget removed."
}

case "${1:-setup}" in
    setup)    setup    ;;
    teardown) teardown ;;
    *)
        echo "Usage: $0 [setup|teardown]"
        exit 1
        ;;
esac
