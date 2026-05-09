# Xbox 360 Auth Donor Strategy

This document describes a practical path for Xbox 360 support where the likely blocker is console authentication hardware behavior.

## Summary

- Yes, a donor approach is viable.
- No, chip-only transplant is usually not the first choice.
- Best first path: keep a full Xbox Toy Pad donor board intact and use it as an auth coprocessor while emulating/bridging the rest.

## Why this matters

For PS/Wii-style devices, HID protocol emulation can be enough for core gameplay behavior. For Xbox 360, there is often additional authentication behavior at the USB/peripheral layer. If that behavior is tied to a dedicated auth element or tightly coupled board logic, pure software emulation can be significantly harder.

## Three integration options (recommended order)

1. Full donor board reuse (recommended)
- Keep Xbox donor Toy Pad board intact.
- Treat it as a black-box auth/peripheral block.
- Bridge your RP2040/ESP32 logic around it.
- Pros: lowest hardware risk, fastest path to first success.
- Cons: larger physical assembly.

2. Auth island transplant (conditional)
- Move the suspected auth IC plus immediate support network (clock, decoupling, reset pull parts, related EEPROM if present).
- Pros: smaller final hardware than full donor board.
- Cons: medium to high rework complexity; still risk of missing dependencies.

3. Chip-only transplant (last resort)
- Move only the auth chip onto your custom board.
- Pros: smallest footprint.
- Cons: highest failure risk; often blocked by hidden dependencies.

## Recommended architecture with your current project

Use a 4-block console-side design for Xbox mode:

1. RP2040
- USB-facing emulation/bridge controller.
- Profile switch: `PROFILE_PLAYSTATION` and `PROFILE_XBOX360`.

2. Console ESP32
- Existing Wi-Fi/UART bridge role remains unchanged.

3. Xbox donor auth block
- Full donor board preferred first.
- Connected non-destructively for traffic observation before any intrusive changes.

4. Pad ESP32
- Existing physical Toy Pad sensing and event forwarding remains unchanged.

## Non-destructive validation plan (do this before desoldering)

1. Baseline capture
- Record USB traffic for stock Xbox setup during boot, idle, tag place/remove, and LED changes.

2. Donor line mapping
- Identify donor board rails, reset, clocks, and data buses.
- Confirm logic levels are 3.3 V before attaching probes/MCUs.

3. Passive sniff
- Attach high-impedance logic analyzer only.
- Confirm reproducible traffic signatures.

4. Bridge rehearsal
- Keep donor board active and intact.
- Verify your RP2040/ESP32 can run in parallel without disturbing donor behavior.

5. Controlled intervention
- Only after stable captures: isolate one signal path at a time.
- Re-test after each change.

## Decision gate for transplant

Proceed to auth island or chip transplant only if all are true:

1. Required auth behavior is isolated to a clear, repeatable subcircuit.
2. Clock/reset dependencies are fully identified.
3. You can reproduce required handshake behavior in controlled tests.
4. You have at least one spare donor board for failure recovery.

## Risk and effort estimate

1. Full donor-board reuse
- Difficulty: medium
- Typical effort: 1 to 2 weeks to first working prototype

2. Auth island transplant
- Difficulty: medium-high
- Typical effort: 2 to 4 weeks

3. Chip-only transplant
- Difficulty: high
- Typical effort: 3 to 6+ weeks

## Practical recommendation

For first Xbox 360 success, prioritize:

1. Full donor board reuse.
2. Capture-driven protocol diffing against PS profile.
3. Profile-gated RP2040 behavior updates.

After first stable success, optimize hardware footprint by attempting auth-island reduction.

## Jailbroken Xbox 360 alternative path

For JTAG/RGH-modded (jailbroken) Xbox 360 consoles, there is an optional alternative workflow documented by the modding community:

- https://consolemods.org/wiki/Xbox_360:Using_Modern_Controllers

How this can help this project:

1. It can reduce strict dependence on stock controller/auth pathways in some setups.
2. It can be useful as a test harness while Xbox Toy Pad auth bring-up is still in progress.
3. It provides a practical fallback path for users who already run homebrew dashboards/plugins.

Important scope note:

1. This is an optional, console-modded path and not a replacement for stock-console-compatible Xbox Toy Pad support.
2. Keep `PROFILE_XBOX360` donor-auth work as the primary path for broad compatibility.
