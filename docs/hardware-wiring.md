# Hardware Wiring Guide

## Overview

Three physical assemblies need to be wired:

| Assembly | Boards | Power source |
|----------|--------|--------------|
| **Console side** | XIAO RP2040 + Console ESP32 | USB port of PS4 / Xbox 360 |
| **Pad side** | Pad ESP32 inside physical Toy Pad | Any USB 5 V supply (charger / power bank) |

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

## 2. Pad Side — Physical Toy Pad internals + Pad ESP32

### 2a. Understanding the Toy Pad PCB

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

### 2b. Recommended wiring strategy: bypass the original MCU

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

### 2c. Pin reference — Pad ESP32 ↔ NFC chip

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

### 2d. LEDs — WS2812 or custom driver

The Toy Pad RGB zones are typically driven by a WS2812B-compatible chain or an
SPI-based LED driver.  

- If the harness has a **single data wire** per zone (or a daisy-chain), it is
  likely WS2812/NeoPixel → connect to `GPIO4` and use the `Adafruit NeoPixel`
  or `FastLED` library.  
- If there are 3 wires per LED cluster (R, G, B + common), it is a simple PWM
  setup — drive with 3× `ledcWrite` channels on ESP32.

---

## 3. Power Supply for the Pad Side

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

## 4. Summary — full system wiring

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
