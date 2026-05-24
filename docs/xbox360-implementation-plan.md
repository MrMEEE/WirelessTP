# Xbox 360 Implementation Plan

This document captures the full analysis for adding Xbox 360 support to WirelessTP, covering
protocol differences, authentication, USB descriptor changes, report framing, the physical
mode-switch design, and implementation order.

No firmware changes are made yet. This document serves as the design record.

---

## 1. LEGO Dimensions Xbox 360 Protocol Findings

### No LEGO Dimensions–specific Xbox 360 documentation exists

An exhaustive search of public sources (GitHub, wiki.tresni, marijnkneppers.dev, brandonw.net,
portal_of_flipper, and related projects) produced no LEGO Dimensions–specific Xbox 360 USB
protocol documentation.

### Command layer is identical across all platforms and games

The toys-to-life command layer used by LEGO Dimensions is the same as Skylanders across PS3,
Xbox 360, Wii, and PC:

| Command | Byte 0 | Purpose |
|---------|--------|---------|
| Ready   | `R` / `0x52` | Portal identity query |
| Activate | `A` / `0x41` | Activate/deactivate |
| Status  | `S` / `0x53` | Figure presence poll |
| Color   | `C` / `0x43` | LED RGB set |
| Query   | `Q` / `0x51` | NFC block read |
| Write   | `W` / `0x57` | NFC block write |
| Light   | `L` / `0x4C` | Zone lighting (LEGO Dimensions) |
| Join    | `J` / `0x4A` | Join confirmation |
| Minifig | `M` / `0x4D` | Minifig metadata |
| B0/B1/B3/C0/D2 | — | Portal auth & NFC ops (LEGO-specific) |

All reports are 32 bytes, zero-padded. Report 0 byte is the command letter (echo back in responses).

### Conclusion for this project

The Xbox 360 LEGO Dimensions portal almost certainly uses identical report payloads to the PS3
version. The only differences are:

1. The USB device class/descriptor set (Xbox 360 uses a custom vendor class, not HID)
2. XSM3 hardware security authentication on the USB connection
3. The framing wrapper on interrupt IN/OUT reports

Per decision during analysis: **presume no protocol difference between platforms.**

### Unknown: Xbox 360 LEGO Dimensions VID:PID

| Console | VID    | PID    | Source |
|---------|--------|--------|--------|
| PS3     | 0x0E6F | 0x0241 | Confirmed via USB capture (this project) |
| Xbox 360 | 0x0E6F | **?** | Not documented publicly |

The manufacturer is PDP Limited (VID 0x0E6F) for both consoles. The Xbox 360 PID is unknown
and would require a USB capture from real Xbox 360 LEGO Dimensions hardware to confirm.
As a starting point, the same VID can be used while the PID is left as a placeholder.

Reference: The Skylanders Xbox 360 portal uses VID 0x1430, PID 0x1F17, which is a
*different manufacturer* — not applicable to LEGO Dimensions.

---

## 2. Xbox 360 USB Architecture

### PS3 (current)

```
bDeviceClass = 0x00  (per-interface)
bNumInterfaces = 1

Interface 0: HID, bInterfaceClass=0x03
  EP 1 IN  (interrupt, 32 bytes, 1ms)
  EP 1 OUT (interrupt, 32 bytes, 1ms)
```

USB strings:
- Manufacturer: `PDP LIMITED. `
- Product: `LEGO READER V2.10`
- Serial: `P.D.P.000000`

### Xbox 360

Xbox 360 uses a custom vendor class (not HID). The descriptor structure follows the standard
Xbox 360 wired controller pattern, which must be replicated exactly for the console to communicate:

```
bDeviceClass = 0xFF  (vendor-specific)
bNumInterfaces = 4

Interface 0: Gamepad/data  (bInterfaceSubClass=0x5D, bInterfaceProtocol=0x01)
  EP 1 IN  (interrupt, up to 32 bytes)
  EP 2 OUT (interrupt, up to 32 bytes)

Interface 1: Headset audio IN  (bInterfaceSubClass=0x5D, bInterfaceProtocol=0x03)
  (minimal/stub for portal use)

Interface 2: Headset audio OUT  (bInterfaceSubClass=0x5D, bInterfaceProtocol=0x02)
  (minimal/stub for portal use)

Interface 3: XSM3 Security  (bInterfaceSubClass=0x5D, bInterfaceProtocol=0x13)
  No endpoints — auth via control requests only
  String descriptor 4: "Xbox Security Method 3, Version 1.00, 2005 Microsoft Corporation. All rights reserved."
```

The string descriptor on interface 3 is critical. Without it, the Xbox 360 console will not
attempt further communication.

Reference: [brandonw.net/360bridge/doc.php](https://www.brandonw.net/360bridge/doc.php),
[portal_of_flipper](https://github.com/sanjay900/portal_of_flipper)

---

## 3. XSM3 Security Authentication

### Overview

Xbox 360 uses a hardware security mechanism (Xbox Security Method 3 / XSM3) that prevents
unauthorized USB peripherals. The console issues two sequential challenges; the device must
respond correctly or all communication stops.

### Library

**libxsm3** by InvoxiPlayGames: [github.com/InvoxiPlayGames/libxsm3](https://github.com/InvoxiPlayGames/libxsm3)

**MrMEEE's RP2040 fork**: [github.com/MrMEEE/libxsm3](https://github.com/MrMEEE/libxsm3)
- License: LGPL-2.1
- Pure C implementation
- Adds RP2040/Pico embedded platform fixes (no heap dependency on init path)
- Implements DES + SHA-1 + Microsoft's PARVE algorithm using retail keys embedded in the library
- Tested working on RP2040

### Control request sequence (interface 3)

All requests are standard USB device/interface control transfers on interface 3:

| bRequest | Direction | Operation |
|----------|-----------|-----------|
| `0x81` | IN  | Get static ID data → call `xsm3_id_data_ms_controller()` |
| `0x82` | OUT | Receive challenge init → call `xsm3_do_challenge_init()` |
| `0x83` | IN  | Send challenge response → call `xsm3_challenge_response()` |
| `0x84` | —   | Challenge-success notification (no action needed) |
| `0x86` | IN  | Get status → return `[0x01, 0x00]` (busy) or `[0x02, 0x00]` (ready) |
| `0x87` | OUT | Receive challenge verify → call `xsm3_do_challenge_verify()` |

### Initialization

```c
xsm3_state_t xsm3;
xsm3_set_vid_pid(&xsm3, USB_VID, USB_PID);   // use portal's actual VID:PID
xsm3_initialise_state(&xsm3);
```

### Key category

The libxsm3 key set used is `xsm3_id_data_ms_controller` (wired controller keys). This is
the same key category used by portal_of_flipper for the Skylanders portal on Xbox 360, and
it works in practice. LEGO Dimensions Xbox 360 portal should use the same category.

### Integration point

XSM3 runs entirely on the console-rp2040. No changes to pad-esp32, console-esp32, or the
link protocol are needed for auth. The RP2040 handles all XSM3 control requests locally.

---

## 4. Report Framing Differences

### PS3

Raw 32-byte HID interrupt transfers:
```
OUT [00..31]: command payload (A/R/S/Q/W/L etc.)
IN  [00..31]: response payload
```

### Xbox 360

Reports use a 2-byte header prefix on the Xbox 360 vendor class:
```
OUT: [0x00, 0x14, payload bytes 2..31]   (host → portal)
IN:  [0x0B, 0x14, payload bytes 2..31]   (portal → host)
```

The first byte identifies the report type:
- `0x00` / `0x0B` = data (portal command/response)
- `0x0B, 0x17` = audio (ADPCM; not used by LEGO Dimensions portal)

**The 32-byte command payload itself is unchanged** — only the 2-byte framing prefix differs.
The gating of this framing is the primary firmware change needed in console-rp2040 for Xbox 360
report handling.

---

## 5. Audio / Headset

Xbox 360 controllers expose headset interfaces (interfaces 1 and 2). For a portal device:
- Audio is never used by the game
- The interfaces must exist in the descriptor to satisfy the console enumeration
- No audio data needs to be handled; stub/minimal descriptors suffice

LEGO Dimensions has no audio output through the portal on any platform.

---

## 6. Chips Affected by Xbox 360 Support

| Component | Change needed |
|-----------|---------------|
| console-rp2040 | Yes — USB descriptor, XSM3 auth, report framing, mode select GPIO |
| console-esp32 | No |
| pad-esp32 | No |
| Link protocol (LP) | No |
| Physical Toy Pad | No |

All Xbox 360-specific logic is isolated to console-rp2040.

---

## 7. Physical Mode Switch on console-rp2040 (XIAO RP2040)

### Motivation

A physical toggle switch allows selecting PS3 or Xbox 360 mode at any time without reflashing.
This is useful for development (quickly testing both paths) and for end-user builds where
the same hardware should support both consoles. The switch is read at boot and also monitored
continuously at runtime — a change triggers a USB re-enumeration with the new profile.

### XIAO RP2040 Pin Availability

| Pin | Function | Available? |
|-----|----------|-----------|
| D0  | GPIO / ADC | **Yes — use for switch** |
| D1  | GPIO / ADC | **Yes — use for status LED** |
| D2  | GPIO / ADC | Yes |
| D3  | GPIO / ADC | Yes |
| D4  | I2C SDA | Yes (if I2C not used) |
| D5  | I2C SCL | Yes (if I2C not used) |
| D6  | UART TX → console ESP32 | **In use** |
| D7  | UART RX ← console ESP32 | **In use** |
| D8  | SPI SCK | Yes (if SPI not used) |
| D9  | SPI MISO | Yes (if SPI not used) |
| D10 | SPI MOSI | Yes (if SPI not used) |
| 11  | NeoPixel enable | Reserved (on-board LED) |
| 12  | NeoPixel data | Reserved (on-board LED) |

**Recommended pins: D0** for the switch, **D1** for the status LED.

### Hardware Design

```
XIAO RP2040 D0 ──── [SPDT or SPST toggle switch] ──── GND

XIAO RP2040 D1 ──── [330 Ω resistor] ──── [LED anode]
                                           [LED cathode] ──── GND
```

Switch:
- Enable internal pull-up (`INPUT_PULLUP`) in firmware
- Switch **open** → pin reads HIGH → PS3 mode
- Switch **closed** (to GND) → pin reads LOW → Xbox 360 mode
- No external resistor needed (internal pull-up is sufficient)

Status LED:
- **ON** (D1 HIGH) → PS3 mode active
- **OFF** (D1 LOW) → Xbox 360 mode active
- 330 Ω current-limiting resistor (3.3 V drive, ~5–10 mA through a standard LED)
- Any colour works; blue pairs visually with the NeoPixel PS indicator

An SPDT center-off or a standard SPST toggle both work. A slide switch on the PCB or a panel
switch in the enclosure are both viable.

### Firmware Behaviour

**At boot (`setup()`):**
1. `pinMode(D0, INPUT_PULLUP)` — switch input.
2. `pinMode(D1, OUTPUT)` — status LED output.
3. Read D0 before USB initialisation.
4. Select `PROFILE_PLAYSTATION` (HIGH) or `PROFILE_XBOX360` (LOW).
5. Write D1: `HIGH` for PS3, `LOW` for Xbox 360.
6. Load the appropriate USB descriptor set and auth handler.
7. Record the initial pin state as `lastSwitchState`.

**At runtime (`loop()`):**
1. Poll D0 on every iteration (no interrupt needed — the loop is fast enough).
2. Debounce: require the new state to be stable for ~50 ms before acting.
3. If the debounced state differs from `lastSwitchState`:
   a. Update `lastSwitchState` and the active profile.
   b. Call `TinyUSBDevice.detach()` to disconnect from the host.
   c. Tear down any XSM3 state (re-call `xsm3_initialise_state()` if switching to/from Xbox).
   d. Wait ~500 ms (host de-enumeration time).
   e. Re-configure the USB descriptors for the new profile.
   f. Call `TinyUSBDevice.attach()` to re-enumerate with the new identity.
4. Write D1: `HIGH` (PS3) or `LOW` (Xbox 360).
5. Update the NeoPixel to the new profile colour.

The host treats this as a physical unplug/replug. The game will need to re-detect the portal,
which is the same behaviour as physically removing and reconnecting it.

### LED Mode Indication (NeoPixel)

The XIAO RP2040 has a WS2812 NeoPixel (data on pin 12, power enable on pin 11).
This can indicate the active profile:

| Profile | NeoPixel colour |
|---------|----------------|
| PS3 | Blue (PlayStation brand colour) |
| Xbox 360 | Green (Xbox brand colour) |

Keep the NeoPixel lit continuously to show the active mode. During the USB detach/reattach
transition, briefly flash the new colour to signal the switch. This is optional and can be
skipped on first implementation.

---

## 8. Implementation Order (when ready to code)

1. **Add libxsm3 as a submodule** under `firmware/console-rp2040/lib/libxsm3/`
   - Use MrMEEE's RP2040 fork: `github.com/MrMEEE/libxsm3`
   - Add to `platformio.ini` `lib_deps` in the `console_rp2040` environment

2. **Add Xbox 360 USB descriptor block** in `console-rp2040/src/main.cpp`
   - 4-interface vendor-class descriptor, gated on `TOY_PROFILE_XBOX360`
   - Confirm Xbox 360 LEGO Dimensions VID:PID (hardware capture or community source)

3. **Add XSM3 control-request handler** for interface 3
   - Handle `0x81 / 0x82 / 0x83 / 0x84 / 0x86 / 0x87` on the security interface
   - Call `xsm3_initialise_state()` at USB mount

4. **Add report framing** in the IN/OUT report path
   - Gate `[0x0B, 0x14, ...]` prefix on `TOY_PROFILE_XBOX360`
   - Strip the 2-byte header from OUT reports before passing to the command handler

5. **Add GPIO for switch (D0) and status LED (D1)**
   - `pinMode(D0, INPUT_PULLUP)` + `pinMode(D1, OUTPUT)` before USB init
   - Read at boot: `profile = (digitalRead(D0) == LOW) ? PROFILE_XBOX360 : PROFILE_PLAYSTATION`
   - `digitalWrite(D1, profile == PROFILE_PLAYSTATION ? HIGH : LOW)`
   - Poll D0 in `loop()` with 50 ms debounce; on stable change: detach USB, reset XSM3 state,
     reconfigure descriptors, reattach USB, update D1

6. **Optional: NeoPixel mode indicator**
   - Show green (Xbox) or blue (PS) continuously; flash briefly during switch transition

7. **Verify Xbox 360 LEGO Dimensions VID:PID**
   - Capture USB traffic from real Xbox 360 + LEGO Dimensions hardware
   - Update descriptor constants once confirmed

---

## 9. References

- [brandonw.net/360bridge/doc.php](https://www.brandonw.net/360bridge/doc.php) — Xbox 360 XSM3 protocol detail
- [github.com/InvoxiPlayGames/libxsm3](https://github.com/InvoxiPlayGames/libxsm3) — libxsm3 (canonical)
- [github.com/MrMEEE/libxsm3](https://github.com/MrMEEE/libxsm3) — RP2040 fork
- [github.com/sanjay900/portal_of_flipper](https://github.com/sanjay900/portal_of_flipper) — validated PS + Xbox 360 portal emulator on Flipper Zero (reference implementation)
- [github.com/tresni/PoweredPortals/wiki/USB-Protocols](https://github.com/tresni/PoweredPortals/wiki/USB-Protocols) — Skylanders command layer reference
- `docs/xbox360-auth-donor-strategy.md` — donor board auth fallback strategy
- `docs/platform-support.md` — PlatformIO environment design, profile model
