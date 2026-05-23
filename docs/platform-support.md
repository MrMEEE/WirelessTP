# Platform Support Strategy

## Current priority

- PlayStation Toy Pad compatibility first.
- Verify end-to-end flow on PlayStation profile before adding Xbox-specific behavior.

## Why staged support

- USB identity details may differ between PlayStation and Xbox 360 Toy Pads.
- Xbox 360 can require additional behavior that should be implemented after baseline is stable.

## Planned approach

1. Implement RP2040 USB descriptors and control/report behavior for PlayStation target.
2. Capture and compare traffic against real PlayStation Toy Pad.
3. Add profile switch in RP2040 firmware for Xbox 360 mode.
4. Capture and compare traffic against real Xbox 360 Toy Pad.
5. Add profile-specific descriptor/report/timing differences.
6. If Xbox auth behavior blocks pure emulation, use donor-board auth strategy in `docs/xbox360-auth-donor-strategy.md`.

## Firmware profile model

Use a runtime profile enum in RP2040 firmware:

- `PROFILE_PLAYSTATION`
- `PROFILE_XBOX360`

The profile should gate:

- USB descriptors
- report format details
- control request handling quirks
- timing/heartbeat behavior if needed

PlatformIO build environment:

- Single unified build: `console_rp2040` (profile selected at runtime via GPIO switch)

The compile-time `TOY_PROFILE_XBOX360` flag has been removed; both profiles are compiled
in and the active profile is determined by reading the mode switch GPIO at boot and
continuously at runtime.

## Xbox 360 strategy note

- For first Xbox milestone, prefer full donor-board auth reuse over chip-only transplant.
- Move to auth-island/chip transplant only after non-destructive captures and repeatable handshake behavior are confirmed.
- Detailed workflow: `docs/xbox360-auth-donor-strategy.md`
- Optional jailbroken-console path (JTAG/RGH) is tracked in `docs/xbox360-auth-donor-strategy.md` via consolemods guidance.
