"""
PlatformIO extra build script.

Patches the Adafruit TinyUSB Library's tusb_config_rp2040.h to add a
#ifndef guard around CFG_TUD_HID_EP_BUFSIZE so the build flag
-DCFG_TUD_HID_EP_BUFSIZE=32 actually takes effect.

Without this patch the header unconditionally defines EP_BUFSIZE=64,
overriding the command-line -D flag.  With EP_BUFSIZE=64, the RP2040 USB
controller pre-arms the HID OUT endpoint expecting 64 bytes; when RPCS3
sends 32-byte reports the XFER_COMPLETE interrupt never fires, so
onHidSetReport() is never called and the device never replies to B0.
"""

import os
Import("env")  # noqa: F821 — PlatformIO global

OLD = "// HID buffer size Should be sufficient to hold ID (if any) + Data\n#define CFG_TUD_HID_EP_BUFSIZE 64\n"
NEW = "// HID buffer size Should be sufficient to hold ID (if any) + Data\n#ifndef CFG_TUD_HID_EP_BUFSIZE\n#define CFG_TUD_HID_EP_BUFSIZE 64\n#endif\n"


def _patch_tusb_config():
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
    env_name = env["PIOENV"]
    config_path = os.path.join(
        libdeps, env_name,
        "Adafruit TinyUSB Library",
        "src", "arduino", "ports", "rp2040", "tusb_config_rp2040.h",
    )
    if not os.path.isfile(config_path):
        print(f"[extra_script] WARNING: tusb_config_rp2040.h not found at {config_path}")
        return
    with open(config_path, "r") as f:
        content = f.read()
    if OLD in content:
        content = content.replace(OLD, NEW)
        with open(config_path, "w") as f:
            f.write(content)
        print(f"[extra_script] Patched {config_path}: added #ifndef guard for CFG_TUD_HID_EP_BUFSIZE")
    elif NEW in content:
        print(f"[extra_script] {config_path} already patched.")
    else:
        print(f"[extra_script] WARNING: expected pattern not found in {config_path}")


_patch_tusb_config()
