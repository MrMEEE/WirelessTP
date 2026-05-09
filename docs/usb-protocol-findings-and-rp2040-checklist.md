# USB Protocol Findings and RP2040 Implementation Checklist

This document captures practical findings from:

- https://github.com/woodenphone/lego_dimensions_protocol
- https://wasabifan.github.io/ev3dev.github.io/docs/tutorials/using-lego-dimensions-toy-pad/

The goal is to turn those findings into concrete RP2040 firmware tasks.

## Confirmed protocol facts (PS profile baseline)

1. USB identity and endpoints
- VID:PID `0x0e6f:0x0241`
- HID interface
- Interrupt OUT endpoint `0x01`, 32-byte packets
- Interrupt IN endpoint `0x81`, 32-byte packets

2. Packet shape
- Byte 0: `0x55` magic
- Byte 1: payload length (logical command length)
- Byte 2: command byte (OUT packets)
- Byte 3: message counter (commonly incrementing)
- Final non-zero byte before padding: checksum
- Packet is padded to 32 bytes with zeroes

3. Checksum
- 1-byte overflow add over command bytes (sum modulo 256)

4. Known startup command
- Startup command bytes before padding:
  - `55 0f b0 01 28 63 29 20 4c 45 47 4f 20 32 30 31 34 f7`
- Then zero padding to 32 bytes

5. Known LED-related commands (OUT endpoint)
- `0xc0`: immediate color one/all pads
- `0xc2`: fade one/all pads
- `0xc3`: flash one/all pads
- `0xc6`: fade all three pads independently
- `0xc7`: flash all three pads independently
- `0xc8`: set all three pads independently

6. NFC event direction hints
- IN endpoint (`0x81`) carries tag-related traffic
- Tutorials and notes indicate tag event packets that can begin with `0x56`
- Fields typically include pad index, action (insert/remove), and UID bytes

7. Xbox caveat
- Both references focus on PS/Wii-type portal behavior
- Xbox variant is noted as different and not covered by those scripts

## Additional findings from Proxmark thread

Source:
- https://www.proxmark.io/www.proxmark.org/forum/viewtopic.php?id=2657&p=2

1. Internal hardware observations (matches current board investigation)
- Main MCU in the LPC11U2x/U3x family
- NFC frontend reported as MFRC630
- LED driving assisted by HEF4049B buffer logic
- Three antennas are present and switched by the MCU

2. HID message families are reinforced
- Host -> portal commands: `0x55, len, cmd, msg#, payload..., checksum, pad`
- Portal -> host replies: `0x55, len, msg#, payload..., checksum, pad`
- Tag events: `0x56, len, zone, ..., action, uid..., checksum, pad`

3. Tag-event example (captured)
- `56 0b 03 00 00 00 04 2a b5 d2 a2 40 80 7b ...`
- Interpreted in-thread as:
  - `03` zone right (center `01`, left `02`)
  - action byte `00` for placed (`01` removed)
  - 7-byte UID follows

4. `0xD2` / `0xD3` command clues
- `0xD2` appears tied to tag read requests
- `0xD3` appears tied to tag write requests
- Example request: `55 04 d2 08 00 26 59`
- Example response family discussed: `55 12 ...` with returned page-group data

5. Important session/authentication clue
- Replaying only `B0` + basic commands can still be insufficient for protected reads/writes
- Thread reports `B1` and `B3` vary per connection and likely participate in a challenge-response/session unlock path
- Practical implication: full tag read/write behavior likely requires reproducing dynamic handshake behavior, not static command replay only

6. Debug port observations
- Pads near `J2` are reported to map to reset/SWD-style signals rather than classic JTAG pins
- LPC11Uxx notes in-thread: JTAG is not the normal debug path, SWD is; access can be limited by code-read-protection settings

## What this changes in our implementation plan

1. Keep PS profile as primary baseline
- Existing HID parser and LED command plan remains correct

2. Split RP2040 scope into two phases
- Phase A: emulate LED + tag event behavior used during normal gameplay (no protected tag write path)
- Phase B: implement or proxy advanced `D2/D3` behavior only after handshake/session behavior is characterized

3. Add explicit backlog items
- Track `B1/B3` dynamic exchange and message sequencing
- Add capture-driven tests for `D2` read path with and without fresh session handshake
- Keep `D3` write support gated behind validated auth/session model

## Practical near-term test updates

Add these tests to bring-up:

1. Tag event conformance test (`0x56`)
- Verify zone/action/uid mapping exactly matches known captures

2. Session freshness test
- Reconnect emulated portal and verify dynamic fields for `B1/B3` are accepted by host

3. `D2` probe test (read path)
- Confirm whether host requests produce valid portal response without protected-page workflow

4. Strict non-goal for first milestone
- Do not block first playable prototype on `D3` write support

## Scope boundaries for this repo

- These findings are ideal for the console-facing USB emulation layer on RP2040.
- They do not fully solve internal Toy Pad PCB takeover between LPC11U24 and onboard NFC chips.

## RP2040 implementation checklist (do this order)

1. Build PS profile USB baseline
- Implement USB descriptors for PS profile
- Expose HID interface with EP OUT `0x01` and EP IN `0x81`
- Fix report size at 32 bytes

2. Add packet codec in RP2040 firmware
- Parse 32-byte OUT packet with validated `0x55` magic
- Extract length, command, counter, args, checksum
- Recompute checksum and reject invalid packets

3. Implement command dispatcher (first 5 commands)
- `0xb0` startup handling
- `0xc0` immediate single/all pad color
- `0xc8` per-pad immediate color set (center/left/right)
- `0xc2` single-pad fade/pulse
- `0xc3` single-pad flash

4. Add IN endpoint reply scaffolding
- Keep a periodic/status packet path for EP `0x81`
- Add a queue for synthetic NFC events coming from ESP32 bridge

5. Bridge integration points
- Map ESP32 `LP_MSG_TAG_SET` and `LP_MSG_TAG_CLEAR` into HID IN event packets
- Map USB LED commands to internal LED state structure and forward to ESP32 side

6. Conformance tests
- Verify startup sequence accepted by game
- Verify checksum handling with known test vectors
- Verify LED commands `c0` and `c8` round-trip through ESP32 pad side
- Verify tag insert/remove path through IN packets

## Suggested parser and state structures (C/C++)

Use small fixed-size structures to keep timing deterministic.

```c
typedef struct {
  uint8_t raw[32];
  uint8_t length;
  uint8_t command;
  uint8_t counter;
  const uint8_t* args;
  uint8_t args_len;
  uint8_t checksum;
  bool valid_magic;
  bool valid_checksum;
} ToyPadOutPacket;

typedef struct {
  bool enable;
  uint8_t r;
  uint8_t g;
  uint8_t b;
} PadRgb;

typedef struct {
  PadRgb center;
  PadRgb left;
  PadRgb right;
  uint8_t last_counter;
  bool initialized;
} ToyPadState;
```

Checksum helper:

```c
static uint8_t toy_checksum(const uint8_t* bytes, uint8_t len) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < len; ++i) {
    sum += bytes[i];
  }
  return (uint8_t)(sum & 0xff);
}
```

## Immediate next coding steps in this repo

1. In `firmware/console-rp2040/src/main.cpp`
- Add a 32-byte USB packet parser and checksum validator
- Add command switch for `0xb0`, `0xc0`, `0xc8`, `0xc2`, `0xc3`

2. Add RP2040 USB profile constants
- Add VID/PID, endpoint IDs, and report size constants
- Keep them behind `PROFILE_PLAYSTATION` for now

3. Add a minimal test harness path
- On valid `0xc0` and `0xc8`, print parsed values to serial for first validation
- Then wire these fields into bridge forwarding to console ESP32

## Traceability notes

These findings are reverse-engineering references used for interoperability and testing. Keep this file updated when new captures are validated, especially for:

- Xbox profile differences
- EP `0x81` NFC payload layout confirmation
- command corner cases and timing constraints
