# RPCS3 USB Passthrough Debugging

This document records what was learned debugging RPCS3 failing to use the RP2040-emulated Toy Pad while the real Toy Pad works fine.

---

## Error symptom

RPCS3 console output:
```
libusb: error [submit_bulk_transfer] submiturb failed, errno=16
```

`errno=16` = `EBUSY`. Despite the misleading `submit_bulk_transfer` label in the log, RPCS3 actually uses **interrupt** transfers for this device (it reads the endpoint descriptor and picks `libusb_fill_interrupt_transfer` when `bmAttributes & 0x03 == 3`). The `submit_bulk_transfer` string is an artefact of the log channel name in the older RPCS3 source.

---

## dmesg comparison

### Real Toy Pad — works correctly
```
usb 1-4: New USB device found, idVendor=0e6f, idProduct=0241
usb 1-4: usbfs: interface 0 claimed by usbhid while 'PPU[0x100000f] ' sets config #1
```
One message. RPCS3 then detaches usbhid, claims the interface, and everything proceeds.

### RP2040 — fails
```
usb 1-1.4.4: New USB device found, idVendor=0e6f, idProduct=0241
usb 1-1.4.4: usbfs: interface 0 claimed by usbhid while 'PPU[0x100000f] ' sets config #1
usb 1-1.4.4: usbfs: interface 0 claimed by usbfs while 'PPU[0x100000f] ' sets config #1
usb 1-1.4.4: usbfs: process XXXXX (PPU[0x100000e] ) did not claim interface 0 before use
... (floods every ~1ms)
```
Two claim messages. After RPCS3 detaches usbhid and re-claims (`claimed by usbfs`), PPU[0x100000e] immediately floods `did not claim interface 0 before use` and every `libusb_submit_transfer` returns `EBUSY`.

---

## Root-cause analysis

### The `did not claim interface 0 before use` kernel warning

This is a usbfs warning printed when a process submits a URB to an interface it does not own **in its own file descriptor**. RPCS3 opens one libusb handle for the device, but two PPU threads (`PPU[0x100000e]` and `PPU[0x100000f]`) share it. Only PPU[0x100000f] actually calls `libusb_claim_interface`; PPU[0x100000e] submits transfers from the same handle but the kernel usbfs tracks ownership per-open-fd/per-thread context. **This is the same with the real Toy Pad** — the real pad also gets the first `claimed by usbhid` message and RPCS3 detaches it the same way.

### Why the real Toy Pad doesn't flood

The real Toy Pad has inherent USB round-trip latency (LPC11U2x MCU, full enumeration, real hardware). By the time RPCS3's `set_configuration` + detach + `claim_interface` sequence completes (~1 second of real USB traffic), PPU[0x100000e]'s initialization window has passed and it is waiting, not flooding.

### Why the RP2040 floods

The RP2040 with TinyUSB responds to control requests (including `SET_CONFIGURATION`) almost instantly. This means `libusb_set_configuration` returns very quickly, PPU[0x100000f] rushes into `claim_interface`, but simultaneously PPU[0x100000e] is already past its startup gate and begins submitting interrupt IN transfers to endpoint `0x81` — before `claim_interface` completes. Each of those `libusb_submit_transfer` calls hits `EBUSY` and (per RPCS3's `send_libusb_transfer`) retries in a tight loop, which is what floods dmesg.

---

## Things tried that did NOT fix it

### `tud_set_configuration_cb` delay (up to 2000ms)
```cpp
extern "C" void tud_set_configuration_cb(uint8_t cfg_num) {
  (void)cfg_num;
  delay(2000);
}
```
Reasoning: block `libusb_set_configuration` on the host for long enough that PPU[0x100000f] can finish the claim before PPU[0x100000e] starts. **Ineffective.** Even at 2000ms, the flood still occurs immediately after the claim. The problem is within RPCS3's threading model — PPU[0x100000e] is already queued to submit and fires the moment the device is claimable, regardless of how long configuration took.

### udev HID unbind rule
```
SUBSYSTEM=="hid", ATTRS{idVendor}=="0e6f", ATTRS{idProduct}=="0241", ACTION=="add", \
  RUN{program}+="/bin/sh -c 'echo -n %k > /sys/bus/hid/drivers/hid-generic/unbind 2>/dev/null; \
                              echo -n %k > /sys/bus/hid/drivers/usbhid/unbind 2>/dev/null'"
```
Reasoning: prevent usbhid from ever claiming so RPCS3 doesn't have to detach it. **Ineffective.** The udev RUN fires asynchronously after the uevent; usbhid wins the race every time on a modern kernel. And even if it did unbind usbhid, the PPU[0x100000e] race exists independently.

---

## Descriptor parity — confirmed identical

`usbhid-dump` output is byte-for-byte identical between the real Toy Pad and RP2040:
```
06 00 FF 09 01 A1 01 19 01 29 20 15 00 26 FF 00
75 08 95 20 81 00 19 01 29 20 91 00 C0
```

`lsusb` endpoint descriptors also match:
- `bInterfaceClass 3 HID`, `bInterfaceSubClass 0`, `bInterfaceProtocol 0`
- EP `0x81` IN interrupt, 32 bytes, 1ms interval
- EP `0x01` OUT interrupt, 32 bytes, 1ms interval
- `bmAttributes 0x80` (bus-powered), `MaxPower 250` (500mA)
- `bcdHID 1.00`

So the issue is **not** a descriptor mismatch.

---

## The Berny23/LD-ToyPad-Emulator approach

https://github.com/Berny23/LD-ToyPad-Emulator works with RPCS3. The key reason it avoids this problem: it uses the **Linux USB gadget framework** (`libcomposite` / `dwc2` kernel modules, `/dev/hidg0`) on a Raspberry Pi Zero W. This is a kernel-level USB device implementation with completely different timing characteristics than TinyUSB on the RP2040. It is not a comparable solution — it is a different hardware/software architecture entirely.

Their udev rule is just:
```
SUBSYSTEM=="usb", ATTR{idVendor}=="0e6f", ATTR{idProduct}=="0241", MODE="0666"
```
No HID unbind. They don't need it because the RPi's USB gadget stack happens to have latency characteristics that avoid the RPCS3 race, or because the gadget stack doesn't trigger the same race at all.

---

## Additional diagnostics performed

### `taskset -c 0 rpcs3` — does NOT fix it
Forcing RPCS3 to a single CPU core eliminates true CPU parallelism between PPU threads. The flood and EBUSY still occur. This rules out the hypothesis that the failure is a CPU scheduling race between PPU threads — the issue is not timing-sensitive in that way.

### Serial debug output when RPCS3 connects
The RP2040 serial output (via ESP32 bridge on ttyACM0) shows only `[rp2040] link hello` repeating every 4 seconds. **No B0, C0, or any Toy Pad command ever arrives.** This means RPCS3 never successfully sends an OUT interrupt transfer to endpoint 0x01 — it fails at the interface claim / IN URB submission stage before it ever sends any commands.

---

## Conclusions

### What is ruled out
- Descriptor mismatch (confirmed byte-for-byte identical)
- CPU parallelism race (taskset -c 0 has no effect)
- Timing of SET_CONFIGURATION (delay up to 2000ms has no effect)
- udev HID unbind (no effect)
- Our command handling / B0 reply logic (never reached)

### What the evidence points to

RPCS3 fails with `errno=16 EBUSY` when submitting the initial IN interrupt URB to endpoint `0x81`. Since B0 never arrives, RPCS3 never gets past this stage. The IN URB submission returns EBUSY from the Linux kernel's `proc_do_submiturb`. This happens when `checkintf` → `claimintf` → `usb_driver_claim_interface` fails because another driver holds the interface — most likely usbhid is re-claiming it after RPCS3's detach attempt.

The real Toy Pad does not trigger this because it is real USB hardware with slower physical response times. The Linux gadget emulator (Berny23) does not trigger this because it uses a kernel-level USB device implementation (`libcomposite`/`dwc2`) that integrates directly with the kernel's USB stack, bypassing the usbhid re-claim race entirely.

**TinyUSB on RP2040 is not a kernel-level gadget.** It enumerates as a standard USB HID device, which causes usbhid to bind. The detach/claim/submit sequence that RPCS3 relies on is fundamentally race-prone when the device responds at firmware speed.

### Architecture options going forward

**Option A — Raspberry Pi Zero 2 W with Linux gadget (recommended)**

Replacing the console-side RP2040 with a Pi Zero 2 W running Linux. Use `libcomposite`/`dwc2` gadget exactly like Berny23's project but integrated locally (no VirtualHere network). This is architecturally identical to what Berny23 proved works with RPCS3. The Pi Zero 2 W still fits the hardware budget and can run the rest of the console-side logic.

**Option B — usbmon trace first, then targeted TinyUSB fix**

Capture USB traffic with usbmon to find the exact USB-level failure:
```bash
sudo modprobe usbmon
sudo tshark -i usbmon1 -w /tmp/usbcap.pcap
# then open in Wireshark
```
This would show whether usbhid is re-binding after RPCS3's detach, whether there is a SET_CONFIGURATION race, or whether TinyUSB is doing something unexpected on the IN endpoint after enumeration. Without this trace, any further firmware changes are guesswork.

**Option C — Kernel usbhid permanent unbind via module quirk**

Add a USB quirk to prevent usbhid from ever binding to this VID:PID at boot, giving RPCS3 a clear device with no race:
```bash
# /etc/modprobe.d/no-usbhid-toypad.conf
options usbhid quirks=0x0e6f:0x0241:0x0004
```
`0x0004` = `HID_QUIRK_IGNORE`. This prevents usbhid from binding entirely. The device would still enumerate; RPCS3's libusb passthrough would claim it directly with no detach needed. This removes the detach→re-claim race without any firmware changes.

This is worth trying immediately — it is a one-line config change on the host.
