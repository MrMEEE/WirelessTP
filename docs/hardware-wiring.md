# Hardware Wiring Guide

## Overview

Three physical assemblies need to be wired:

| Assembly | Boards | Power source |
|----------|--------|--------------|
| **Console side** | XIAO RP2040 + Console ESP32 | USB port of PS4 / Xbox 360 |
| **Pad side** | Lolin ESP32-S2 Mini + physical Toy Pad | Any USB 5 V supply (charger / power bank) |

---

## 1. Console Side — XIAO RP2040 + Console ESP32

The RP2040 plugs into the console (PS4/Xbox) via its USB-C port. That USB port
also supplies 5 V for both boards.

```
PS4 / Xbox 360 USB port
        │
        │ USB-C cable
        ▼
┌─────────────────────┐
│   XIAO RP2040       │
│                     │
│  5V ─────────────────────► ESP32 VIN (5 V in)
│  GND ────────────────────► ESP32 GND
│  TX  (Serial1 / D6) ────► ESP32 RX (GPIO16)
│  RX  (Serial1 / D7) ◄─── ESP32 TX (GPIO17)
└─────────────────────┘
```

### Pin reference

| XIAO RP2040 | Signal | ESP32 |
|-------------|--------|-------|
| `5V` (USB pass-through) | Power | `VIN` |
| `GND` | Ground | `GND` |
| `D6` (TX / Serial1) | UART TX | `GPIO16` (U2RXD) |
| `D7` (RX / Serial1) | UART RX | `GPIO17` (U2TXD) |

### Additional pins — console-rp2040 (XIAO RP2040)

These pins are used for the profile switch and status indicators on the XIAO
RP2040 (PS3 vs Xbox 360 USB profile selection):

| XIAO RP2040 pin | GPIO | Signal | Notes |
|-----------------|------|--------|-------|
| `D0` | 26 | Mode switch | `INPUT_PULLUP`; open/HIGH = PS3, pulled LOW = Xbox 360 |
| `D1` | 27 | PS3 status LED | HIGH = on when PS3 profile is active |
| `D2` | 28 | Xbox 360 status LED | HIGH = on when Xbox 360 profile is active |
| `11` | 11 | NeoPixel power enable | Drive HIGH to power the onboard NeoPixel |
| `12` | 12 | NeoPixel data | WS2812, 1 pixel; blue = PS3, green = Xbox 360 |

Connect a discrete LED (with series resistor, e.g. 330 Ω to GND) to each of
`D1` and `D2`. Only one LED is on at a time, immediately reflecting the active
USB profile. The NeoPixel mirrors the same state in colour.

> **Serial port:** The firmware uses `Serial1` on the RP2040 and `Serial2`
> (`HardwareSerial(2)`) on the ESP32 — both at 115 200 baud.

### Power budget — console USB port

| Component | Typical | Peak |
|-----------|---------|------|
| XIAO RP2040 | 40 mA | 100 mA |
| ESP32 (Wi-Fi active) | 80 mA | 240 mA |
| **Total** | **120 mA** | **340 mA** |

PS4 front USB ports supply **500 mA** (USB 2.0); Xbox 360 side ports supply
**500 mA**.  The worst-case 340 mA fits comfortably within that budget.

---

## 2. Pad Side — ESP32-S2 Mini as USB Host

The ESP32-S2 Mini acts as a USB host to the physical Toy Pad.  
The Toy Pad stays **completely unmodified** — no soldering on the Toy Pad PCB.

### 2a. Recommended wiring: USB-A female → USB-C adapter (no soldering)

Use a **USB-A female to USB-C adapter** (or a short USB-A female ↔ USB-C
cable) to connect the toypad's intact USB cable to the S2 Mini's USB-C port.
Power the S2 Mini via its **VBUS + GND header pins** from an external 5 V
supply (power bank, charger, etc.).

```
Physical Toy Pad
  USB-A plug ──► USB-A female ↔ USB-C adapter ──► USB-C port on S2 Mini
                                                         (D− / D+ / VBUS)
External 5 V supply ─────────────────────────────► VBUS header pin on S2 Mini
                    ─────────────────────────────► GND  header pin on S2 Mini
```

> **Why VBUS header pin?**  GPIO 19 (D−) and GPIO 20 (D+) are the ESP32-S2's
> USB OTG lines and are **only** accessible via the USB-C connector — they are
> **not** broken out as solder pads on the header.  The USB-C port is therefore
> used for the toypad data connection.  5 V fed into the `VBUS` header pin
> powers the board and also flows through to the USB-C VBUS, supplying the
> toypad.

### 2b. Alternate wiring: cut cable + USB-C breakout board

If you prefer a permanent direct-wired connection, cut the toypad's USB-A
cable and connect the four wires to a **USB-C breakout board**, then plug
the breakout into the S2 Mini's USB-C port.  The S2 Mini still needs
external power via the VBUS + GND header pins.

```
Physical Toy Pad  USB cable
  Red   (VBUS 5V) ─────────────────► USB-C breakout VBUS
  Black (GND)     ─────────────────► USB-C breakout GND
  White (D−)      ─────────────────► USB-C breakout D−
  Green (D+)      ─────────────────► USB-C breakout D+
                             USB-C breakout ──► S2 Mini USB-C port

External 5 V supply ─────────────────────────► VBUS header pin on S2 Mini
                    ─────────────────────────► GND  header pin on S2 Mini
```

### 2c. Pin reference — S2 Mini ↔ Toy Pad USB

| Signal | ESP32-S2 Mini | Toy Pad USB cable |
|--------|---------------|-------------------|
| USB D− | USB-C connector D− (internal GPIO 19) | White wire |
| USB D+ | USB-C connector D+ (internal GPIO 20) | Green wire |
| 5 V (to toypad) | `VBUS` header pin → USB-C VBUS | Red wire |
| GND | `GND` header pin | Black wire |

> GPIO 19 / GPIO 20 are the ESP32-S2's native USB OTG lines.  On the S2 Mini
> they are wired directly to the USB-C connector and are **not** exposed as
> header pins — the USB-C port is the only way to access them.

### 2d. Power budget — pad side

| Component | Typical | Peak |
|-----------|---------|------|
| ESP32-S2 Mini (WiFi active) | 80 mA | 240 mA |
| Physical Toy Pad (USB powered) | 80 mA | 160 mA |
| **Total** | **160 mA** | **400 mA** |

A USB power bank or any 5 V charger at 500 mA is sufficient.

---

## 2e. Status Indicator and Mode Button — Pad Side

A common-anode RGB LED and a push-button are added to the S2 Mini to show
connection/mode status and allow switching between physical-pad and emulator
mode without a computer.

### Wiring

```
ESP32-S2 Mini header                        Component
─────────────────────────────────────────────────────────────────────
GPIO 11  ───[330 Ω]──► RGB LED Red anode leg
GPIO 12  ───[330 Ω]──► RGB LED Green anode leg
GPIO 13  ───[330 Ω]──► RGB LED Blue anode leg
                        RGB LED Common Anode (+) ──► 3.3 V

GPIO 14  ───────────── Button one terminal
GND      ───────────── Button other terminal
```

> **Common-anode LED:** the shared anode (+) is tied to 3.3 V.
> Driving a pin **LOW** turns that colour **on**; **HIGH** turns it **off**.
> An internal `INPUT_PULLUP` is used for the button so no external resistor
> is needed — pressing the button pulls GPIO 14 to GND (active-low).

### Pin reference — pad-esp32 (ESP32-S2 Mini)

| GPIO | Signal | Direction | Notes |
|------|--------|-----------|-------|
| `11` | RGB Red | Output | LOW = on (common-anode) |
| `12` | RGB Green | Output | LOW = on |
| `13` | RGB Blue | Output | LOW = on |
| `14` | Mode button | Input (PULLUP) | LOW = pressed |

### LED colour states

| Colour | Meaning |
|--------|---------|
| **Yellow** (Red + Green) | Not connected to console |
| **Green** | Connected — physical passthrough mode (physical Toy Pad active) |
| **Magenta** (Red + Blue) | Connected — emulator mode (console-esp32 manages virtual toys) |

### Mode switching

Pressing the button sends a toggle request (`LP_MSG_SET_MODE`) to the
console-esp32 over the existing WiFi/LP link.  The console-esp32 is the
authority on the current mode: it updates its own `runtimeMode`, persists it
to flash, clears stale slot state on the RP2040 if needed, and replies with
the new mode.  The pad-esp32 then updates its LED to reflect the confirmed
mode.

The mode can also be changed at any time from the console-esp32 web UI
(`/api/mode`); the pad LED will update automatically.

---

## 3. Pad Side (Advanced) — Direct MCU bypass

> This section describes bypassing the Toy Pad's internal MCU and wiring
> directly to the NFC chip and LED driver.  This is **not required** with the
> S2 Mini USB host approach above; it is kept for reference.

### 3a. Understanding the Toy Pad PCB

The PS3/PS4 round Toy Pad contains (visible in the PCB photo):

| Part | Role |
|------|------|
| ARM Cortex-M0 MCU (e.g. LPC11U35 or STM32F0) | Original host controller, talks USB HID to console |
| NFC frontend IC (typically **TRF7970A** or **PN532**) | Reads NTAG215 / Mifare tags; connects to MCU via SPI or I2C |
| RGB LED driver (WS2812 chain or discrete driver) | Powers the 3-zone lighting |
| Crystal oscillator | Clock for MCU |
| Multi-wire harness (red/black/green/white wires in photo) | Power + data to satellite boards / LED rings |

> To identify the exact ICs, look for the largest 32-pin (or QFP) package (MCU)
> and the second largest IC near the antenna traces (NFC frontend).  The chip
> markings will confirm the exact model.  Common markings to look for:
> - **TRF7970A** — TI NFC frontend (SPI)
> - **PN532** — NXP NFC controller (I2C, SPI, or UART)
> - **LPC11U35** — NXP ARM M0 (the original host MCU)
> - **STM32F042** — ST ARM M0

### 3b. Bypass wiring strategy

The cleanest approach is to leave the TRF7970A / PN532 in place, disable or
remove the original ARM MCU, and connect the Pad ESP32 directly to the NFC
frontend and LED chain.

```
Physical Toy Pad PCB
                      ┌─────────────────────────┐
              NFC     │  TRF7970A or PN532       │
              antenna ◄──────────────────────────┤
            (coil)    │  SPI / I2C pins           │
                      └───────┬─────┬────┬───────┘
                              │     │    │
              ┌───────────────┘     │    └──────────────────┐
              │                     │                       │
          SPI_MISO               SPI_SCK / SDA            SPI_MOSI / SCL
              │                     │                       │
   ┌──────────▼─────────────────────▼───────────────────────▼──────────┐
   │                       Pad ESP32                                    │
   │                                                                    │
   │  GPIO19 (MISO)    GPIO18 (SCK)    GPIO23 (MOSI)   GPIO5 (CS)     │
   │     ──────── SPI to NFC chip ────────────────────────────         │
   │                                                                    │
   │  GPIO4  ─────────────────────────────────────────► LED data (WS2812) │
   │                                                                    │
   │  5V ◄──── External USB 5V supply                                  │
   │  GND ──── GND (common with Toy Pad PCB GND)                       │
   └────────────────────────────────────────────────────────────────────┘
```

### 3c. Pin reference — Pad ESP32 ↔ NFC chip

#### If NFC chip is **TRF7970A** (SPI mode)

| TRF7970A pin | Signal | ESP32 GPIO |
|---|---|---|
| `MISO` / `DATA_OUT` | SPI MISO | `GPIO19` |
| `MOSI` / `DATA_IN` | SPI MOSI | `GPIO23` |
| `CLK` | SPI clock | `GPIO18` |
| `SS` / `CS` | Chip select | `GPIO5` |
| `MOD` | Modulator (keep HIGH) | 3.3 V via 10 kΩ |
| `IRQ` / `ASK` | Tag present interrupt | `GPIO4` (optional) |
| `VCC` | 3.3 V supply | `3.3 V` |
| `GND` | Ground | `GND` |

Use the [TRF7970A Arduino library](https://github.com/electricimp/TRF7970a) or
Adafruit's NFC library adapted for SPI.

#### If NFC chip is **PN532** (I2C mode)

| PN532 pin | Signal | ESP32 GPIO |
|---|---|---|
| `SDA` | I2C data | `GPIO21` |
| `SCL` | I2C clock | `GPIO22` |
| `IRQ` | Interrupt out | `GPIO4` |
| `RSTO` | Reset (optional) | `GPIO2` |
| `VCC` | 3.3 V | `3.3 V` |
| `GND` | Ground | `GND` |

Use `Adafruit_PN532` library with I2C; set `I2C_ADDRESS = 0x24` (the PN532
default when HSU/SPI pads are left floating).

> **Determining the interface mode:** Most Toy Pad TRF7970A chips are wired in
> SPI.  If you find exactly 4 data traces going from the NFC IC toward the old
> MCU (MOSI, MISO, SCK, CS) it is SPI.  Two traces with pull-ups suggests I2C.

### 3d. LEDs — WS2812 or custom driver

The Toy Pad RGB zones are typically driven by a WS2812B-compatible chain or an
SPI-based LED driver.  

- If the harness has a **single data wire** per zone (or a daisy-chain), it is
  likely WS2812/NeoPixel → connect to `GPIO4` and use the `Adafruit NeoPixel`
  or `FastLED` library.  
- If there are 3 wires per LED cluster (R, G, B + common), it is a simple PWM
  setup — drive with 3× `ledcWrite` channels on ESP32.

---

## 4. Power Supply for the Pad Side

Once the physical Toy Pad is modified it no longer plugs into the specific
console — it just needs **5 V USB power**.  Options, best-first:

| Option | Notes |
|--------|-------|
| USB power bank (slim) | Best — wireless pad is then fully wireless |
| Phone charger / wall adapter | Good for desk setups |
| Toy Pad original USB cable → any USB hub | Re-uses existing cable |
| Second USB port on the same PS4/Xbox | Feasible (see budget below) |

### Using the second USB port on the console

If you want both assemblies powered from the console:

```
Console USB port A ─── XIAO RP2040 (data + power)─── Console ESP32
Console USB port B ─── Pad ESP32 + Toy Pad PCB power
```

PS4 has 2 front USB 2.0 ports, each rated 500 mA.

| Pad-side component | Current |
|--------------------|---------|
| ESP32 (Wi-Fi active) | ~240 mA peak |
| NFC frontend (TRF7970A / PN532) | ~55 mA |
| WS2812 LEDs × 12 @ 20 mA | ~240 mA max (all white) |
| **Total worst case** | **~535 mA** |

That slightly exceeds a single 500 mA port at full LED brightness.  
**Mitigate** by capping LED brightness to 50 % in firmware (`FastLED.setBrightness(128)`) which brings the LED contribution down to ~120 mA, giving a safe total of ~415 mA.

> The Xbox 360 also has two USB 2.0 host ports (500 mA each), so the same
> strategy applies.

---

## 5. Summary — full system wiring

```
      ┌──── Console USB port A (data+power) ────┐
      │                                          │
      ▼                                          ▼
 XIAO RP2040  ◄──UART──►  Console ESP32  )))  ((( Pad ESP32 ◄──► NFC / LEDs
 (USB device)                (Wi-Fi AP)  Wi-Fi     (Wi-Fi STA)
                                                       │
                                       Console USB port B  (power only)
                                       -OR- USB power bank
```

---

## 5. Steps to identify your exact NFC chip

1. Open the Toy Pad (4 screws on the bottom).
2. Under bright light / magnifier, find the IC closest to the NFC coil antenna
   traces on the PCB.
3. Read the top marking — photograph under macro mode if needed.
4. Match to TRF7970A, PN532, or post the marking here for identification.
5. Count the wires going between that IC and the main MCU to confirm SPI (4)
   or I2C (2 + 2 pull-ups).
