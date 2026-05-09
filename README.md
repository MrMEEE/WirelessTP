# Wireless LEGO Dimensions Toy Pad Bridge (RP2040 + ESP32 + ESP32)

This repository is a starter for a 3-board architecture:

- Console dongle USB side: Seeed XIAO RP2040
- Console dongle radio side: ESP32 (classic ESP-WROOM-32 is fine)
- Pad side: ESP32

## Why this split

- RP2040 handles strict USB device timing and Toy Pad emulation.
- Console ESP32 hosts a direct open SoftAP and forwards paired pad events over UART to RP2040.
- Pad ESP32 handles tag sensing / virtual events and sends events over Wi-Fi.
- Pad ESP32 includes local setup AP + web portal for first-time provisioning.

This keeps USB timing isolated from Wi-Fi jitter.

## Current status

- Shared packet format (`shared/include/link_protocol.h`) created.
- Console ESP32 starter firmware added (`firmware/console-esp32/src/main.cpp`).
- Console RP2040 starter firmware added (`firmware/console-rp2040/src/main.cpp`).
- Pad ESP32 starter firmware added (`firmware/pad-esp32/src/main.cpp`).
- First protocol document added (`protocol/toypad_link_v1.md`).
- Framed transport with CRC + ACK + heartbeat is wired in all three targets.
- Bring-up checklist added (`docs/bringup-checklist.md`).
- Direct open-AP pairing flow is implemented (no external router required, no serial pair code entry).
- USB protocol findings + RP2040 checklist documented (`docs/usb-protocol-findings-and-rp2040-checklist.md`).
- Xbox 360 donor-auth integration strategy documented (`docs/xbox360-auth-donor-strategy.md`).

## Hardware wiring (console side)

Use UART between console ESP32 and XIAO RP2040:

- ESP32 TX -> RP2040 RX
- ESP32 RX <- RP2040 TX
- ESP32 GND <-> RP2040 GND

Use 3.3 V logic only.

## Build approach

These are framework starter files (not yet board-package complete project files).

For VS Code + PlatformIO IDE, you can now open the repository root directly because it contains a top-level `platformio.ini` that exposes all firmware targets.

You can also open the workspace file at the repo root:

`WirelessTP.code-workspace`

That workspace adds each firmware target as its own VS Code folder so the PlatformIO extension can also show them as separate projects in one window.

You can use either:

1. Arduino IDE per board, or
2. PlatformIO with one env per target.

For RP2040 profile builds in PlatformIO:

1. PlayStation profile: `console_rp2040_ps`
2. Xbox 360 profile: `console_rp2040_xbox360`

The root-level PlatformIO environments are:

1. `console_esp32`
2. `pad_esp32`
3. `console_rp2040_ps`
4. `console_rp2040_xbox360`

## Next milestones

1. Replace RP2040 placeholder with full TinyUSB Toy Pad descriptor + endpoint handlers.
2. Implement deterministic state map for the 3 pad zones and 7 minifig slots.
3. Implement PS-profile USB command parser/checksum path (`0xb0`, `0xc0`, `0xc8`, `0xc2`, `0xc3`).
4. Add NFC reader integration on pad-side ESP32 (or keep virtual events first).
5. Add LED command flow from console game -> RP2040 -> console ESP32 -> pad ESP32.

## Platform support plan

- PlayStation Toy Pad is the primary target for first functional milestone.
- Xbox 360 support is planned next; expect additional compatibility/auth investigation.
- For Xbox 360, start with donor-board auth reuse before attempting chip-level transplant (`docs/xbox360-auth-donor-strategy.md`).
- For jailbroken Xbox 360 setups (JTAG/RGH), an optional modern-controller workaround path is also documented (`docs/xbox360-auth-donor-strategy.md`).
- RP2040 PlatformIO envs are split by profile: `console_rp2040_ps` and `console_rp2040_xbox360`.
