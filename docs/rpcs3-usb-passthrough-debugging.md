# RPCS3 USB Passthrough Debugging

This document records what was learned debugging RPCS3 failing to recognise
the RP2040-emulated Toy Pad while the real Toy Pad works fine.

---

## Symptom

RPCS3 launches and the Dimensions game never finds the Toy Pad. The device
enumerates correctly (VID:PID 0x0E6F:0x0241, correct descriptors) and
appears in `lsusb`, but the game UI shows no pad connected. RPCS3 re-reads
the configuration descriptor every ~1.7 seconds, indicating it is retrying
after the device fails to respond to its init command.

The RP2040 serial log (via ESP32 bridge on Serial1) prints only:

```
[rp2040] boot profile=ps usb vid=0x0e6f pid=0x0241
[rp2040] usb mounted
[rp2040] b0 stats rx=0 q=0 tx=0   ← repeats every 3s
```

`rx=0` means `onHidSetReport` is never called — the B0 init command is
never received by firmware, even though the USB host confirms the OUT
transfer completed.

---

## USB trace evidence

A `usbmon` capture (`physical-YYYYMMDD.pcapng` vs `virtual-YYYYMMDD.pcapng`)
shows the difference clearly.

### Real Toy Pad (working)

```
→ OUT ep1  55 0f b0 01 ...   [B0 command, 32 bytes]
← IN  ep1  55 19 01 00 2f ... [B0 reply,  32 bytes, arrives ~40ms later]
→ OUT ep1  55 06 c0 01 ...   [C0 command]
← IN  ep1  55 07 c0 01 ...
... full B0→C0→B1→B3→C6 handshake completes
```

### RP2040 (broken, before fix)

```
→ OUT ep1  55 0f b0 01 ...   [B0 command, 32 bytes — USB-level ACK confirmed]
← IN  ep1  (pending, never completed)
                              [RPCS3 waits 1.7s, gives up, re-enumerates]
```

The OUT transfer is acknowledged at the USB level (the host sees it
complete with `len=32`), but TinyUSB's `onHidSetReport` callback never
fires. The RP2040 never queues a B0 reply, so the pending IN URB sits
unanswered forever.

---

## Root cause

`tusb_config_rp2040.h` (Adafruit TinyUSB Library) defines:

```c
// HID buffer size Should be sufficient to hold ID (if any) + Data
#define CFG_TUD_HID_EP_BUFSIZE 64
```

This is an **unconditional** `#define` — no `#ifndef` guard. It is included
by `tusb_config.h`, which is included by every TinyUSB C source file.

Because it has no guard, it overrides the `-DCFG_TUD_HID_EP_BUFSIZE=32`
build flag set in `platformio.ini`. The command-line define is processed
first (at the logical start of each translation unit), but the header
redefines the symbol unconditionally when it is included later, so the
final value is always 64.

With `CFG_TUD_HID_EP_BUFSIZE=64` in effect:

1. TinyUSB calls `usbd_edpt_xfer(rhport, ep_out, buf, 64)` to pre-arm the
   HID OUT endpoint, telling the RP2040 USB DMA to collect 64 bytes.
2. RPCS3 sends a 32-byte OUT interrupt packet. The USB hardware ACKs it
   (the host sees the transfer complete at the USB level).
3. The RP2040 DMA is still waiting for the remaining 32 bytes — it never
   signals `XFER_COMPLETE`.
4. TinyUSB never calls `hidd_xfer_cb`, so `tud_hid_set_report_cb` /
   `onHidSetReport` is never invoked.
5. The B0 reply is never queued. The pending IN URB is never answered.
   RPCS3 times out and retries.

---

## Fix

In `extra_script.py` (run as a PlatformIO pre-build step), patch the
Adafruit TinyUSB Library copy in `.pio/libdeps/` to add the missing
`#ifndef` guard:

```c
#ifndef CFG_TUD_HID_EP_BUFSIZE
#define CFG_TUD_HID_EP_BUFSIZE 64
#endif
```

With the guard in place, the `-DCFG_TUD_HID_EP_BUFSIZE=32` build flag in
`platformio.ini` takes effect. TinyUSB arms the OUT endpoint with 32 bytes,
RPCS3's 32-byte B0 command triggers `XFER_COMPLETE`, `onHidSetReport` fires,
and the B0 reply is sent. RPCS3 completes the full handshake.

The platform is also pinned to
`maxgerhardt/platform-raspberrypi#v1.4.0-gcc14-arduinopico460`
(arduino-pico 4.6.0) to prevent a future automatic upgrade from
reintroducing the mismatch with a different library version.

---

## udev rule (required on host)

RPCS3's libusb passthrough requires the device to be accessible without
root. Add to `/etc/udev/rules.d/50-toypad.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="0e6f", ATTR{idProduct}=="0241", MODE="0666"
```

Then reload rules:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```
