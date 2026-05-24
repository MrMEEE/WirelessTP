# Architecture Decisions

## Why the console side uses two boards (RP2040 + ESP32)

**Question considered:** Can a single ESP32-S2 Mini replace both the console-rp2040
and console-esp32, eliminating the UART link and the extra board?

**Short answer:** Technically possible, but deliberately not done.

### What each board currently does

| Board | Role |
|-------|------|
| XIAO RP2040 | USB HID *device* to game console — emulates the toy pad, handles TEA auth (B1/B3), responds to tag/LED commands |
| Console ESP32 | WiFi AP, web UI, TCP server for pad-esp32, game state (slots/toybox), forwards LP frames to/from RP2040 |

### Why the ESP32-S2 is not a drop-in replacement

1. **USB timing isolation.**
   The RP2040 has no WiFi and a bare-metal FreeRTOS-free loop. Its `loop()` calls
   `TinyUSBDevice.task()` twice per iteration specifically to chain B0 replies within
   one 1 ms USB poll window. On the ESP32-S2, FreeRTOS WiFi tasks (lwIP, beacon
   handling, TCP stack) run on the same or adjacent cores and can introduce multi-ms
   jitter. The B1/B3 TEA auth and the B0 reply timing are sensitive enough that this
   can cause the game console to fail HID enumeration, particularly on real hardware
   (less visible on RPCS3).

2. **RP2040-specific linker tricks cannot be trivially ported.**
   The RP2040 firmware patches the USB configuration descriptor using GCC `--wrap`
   linker symbols (`__wrap_TinyUSB_Device_Init`,
   `__wrap_tud_descriptor_configuration_cb`). These intercept Arduino TinyUSB
   internals at link time. The ESP32-S2 uses the ESP-IDF toolchain; equivalent
   patching would require rewriting these hooks against IDF's `tusb_config` and
   custom descriptor callbacks — a non-trivial porting effort with different testing
   requirements.

3. **`tud_hid_report_complete_cb` chaining.**
   The firmware queues the B0 USB reply inside the HID report-complete callback to
   avoid waiting a full loop cycle. This interrupt-context chaining works reliably on
   RP2040 TinyUSB HAL. Its behaviour on ESP32-S2 TinyUSB (which dispatches USB
   events differently under IDF) has not been validated.

4. **USB serial debug lost.**
   Once the S2's USB OTG port is owned by the game console as a HID device, USB
   serial is gone. All debug output would require a UART adapter on GPIO 43/44.
   On the RP2040 this is a non-issue because the board has a second USB interface
   (XIAO exposes CDC ACM separately from the HID interface).

### Summary

The two-board split exists because the RP2040 provides bare-metal USB timing that
the ESP32-S2 cannot guarantee while simultaneously running a WiFi stack. Merging
them would trade a clean UART interface for a complex, risk-laden port with no clear
gain in the current scope. If timing requirements relax or the protocol is changed
to tolerate jitter, this decision should be revisited.
