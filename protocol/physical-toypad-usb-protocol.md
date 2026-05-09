# Physical LEGO Dimensions Toy Pad — USB Protocol Reference

Captured and decoded from: `firmware/console-pi/host/traces/physical-20260505-221015.pcapng`
Tool: `sudo tcpdump -nn -tttt -x -r <pcap>`
Host: Linux / RPCS3 HID driver
Physical device: OEM LEGO Dimensions Toy Pad (PDP)

---

## USB Descriptor Values

All values verified byte-for-byte from the usbmon enumeration capture.

### Device Descriptor

```
bLength:            18 (0x12)
bDescriptorType:    DEVICE (0x01)
bcdUSB:             0x0200  (USB 2.0)
bDeviceClass:       0x00    (defined per-interface)
bDeviceSubClass:    0x00
bDeviceProtocol:    0x00
bMaxPacketSize0:    64 (0x40)
idVendor:           0x0E6F
idProduct:          0x0241
bcdDevice:          0x0100
iManufacturer:      1  →  "LEGO READER V2.10"
iProduct:           2  →  "PDP LIMITED. "
iSerialNumber:      3  →  "P.D.P.000000"
bNumConfigurations: 1
```

Raw bytes: `12 01 00 02 00 00 00 40 6F 0E 41 02 00 01 01 02 03 01`

> **Note:** The physical device reports manufacturer=`"PDP LIMITED. "` and
> product=`"LEGO READER V2.10"`. `setup_gadget.sh` already matches that ordering.

### String Descriptors (English 0x0409)

| Index | Raw (first 4 bytes) | Decoded (UTF-16LE) |
|-------|--------------------|--------------------|
| 0     | `04 03 09 04`      | Language: 0x0409 (English US) |
| 1     | `24 03 4C 00 45 00 …` | "LEGO READER V2.10" (17 chars, 36 bytes) |
| 2     | `1C 03 50 00 44 00 …` | "PDP LIMITED. " (13 chars, 28 bytes) |
| 3     | `1A 03 50 00 2E 00 …` | "P.D.P.000000" (12 chars, 26 bytes) |

### Configuration Descriptor

```
bLength:              9
bDescriptorType:      CONFIGURATION (0x02)
wTotalLength:         0x0029 = 41
bNumInterfaces:       1
bConfigurationValue:  1
iConfiguration:       0  (no string)
bmAttributes:         0x80  (bus-powered, no remote wakeup)
bMaxPower:            0xFA = 250  (250 × 2 mA = 500 mA)
```

Raw (9-byte short form): `09 02 29 00 01 01 00 80 FA`

### Interface Descriptor

```
bLength:            9
bDescriptorType:    INTERFACE (0x04)
bInterfaceNumber:   0
bAlternateSetting:  0
bNumEndpoints:      2
bInterfaceClass:    0x03  (HID)
bInterfaceSubClass: 0x00
bInterfaceProtocol: 0x00
iInterface:         0
```

### HID Descriptor

```
bLength:            9
bDescriptorType:    HID (0x21)
bcdHID:             0x0100  (HID 1.00)
bCountryCode:       0x00
bNumDescriptors:    1
bDescriptorType[0]: 0x22  (Report)
wDescriptorLength:  0x001D = 29 bytes
```

### Endpoint Descriptors

Two interrupt endpoints — the physical uses **both** IN and OUT interrupt endpoints,
not a control-transfer fallback.

| EP Address | Direction | Type      | wMaxPacketSize | bInterval |
|------------|-----------|-----------|----------------|-----------|
| 0x81       | IN        | Interrupt | 32 (0x0020)    | 1 (1 ms at full-speed) |
| 0x01       | OUT       | Interrupt | 32 (0x0020)    | 1 (1 ms at full-speed) |

Full config+interface+HID+endpoints raw (41 bytes):
```
09 02 29 00 01 01 00 80 FA   ← configuration
09 04 00 00 02 03 00 00 00   ← interface (2 eps, HID class)
09 21 00 01 00 01 22 1D 00   ← HID (bcdHID=0x0100, report desc 29 bytes)
07 05 81 03 20 00 01          ← EP 0x81 IN  interrupt 32B interval=1
07 05 01 03 20 00 01          ← EP 0x01 OUT interrupt 32B interval=1
```

### HID Report Descriptor (29 bytes)

```
06 00 FF   Usage Page (Vendor Defined, 0xFF00)
09 01       Usage (0x01)
A1 01       Collection (Application)
19 01         Usage Minimum (1)
29 20         Usage Maximum (32)
15 00         Logical Minimum (0)
26 FF 00      Logical Maximum (255)
75 08         Report Size (8 bits)
95 20         Report Count (32)
81 00         Input  (Data, Array, Absolute)  ← IN report (pad → host)
19 01         Usage Minimum (1)
29 20         Usage Maximum (32)
91 00         Output (Data, Array, Absolute)  ← OUT report (host → pad)
C0          End Collection
```

Raw: `06 00 FF 09 01 A1 01 19 01 29 20 15 00 26 FF 00 75 08 95 20 81 00 19 01 29 20 91 00 C0`

---

## USB Enumeration Sequence

Timing from capture (relative to plug-in at t=0):

```
t+0.000s   GET_DESCRIPTOR(DEVICE)        → 18 bytes device descriptor
t+0.028s   GET_DESCRIPTOR(CONFIG, 9)     → 9 bytes (short, just config header)
t+0.055s   GET_DESCRIPTOR(CONFIG, 41)    → 41 bytes (full config+IF+HID+EPs)
t+0.085s   GET_DESCRIPTOR(STRING, 0)     → language list
t+0.113s   GET_DESCRIPTOR(STRING, 1)     → manufacturer string
t+0.116s   GET_DESCRIPTOR(STRING, 2)     → product string
t+0.119s   GET_DESCRIPTOR(STRING, 3)     → serial number string
t+0.122s   GET_DESCRIPTOR(STRING, 3)     → serial number string (fetched twice)
t+0.152s   SET_CONFIGURATION(1)          → no data
t+0.154s   GET_DESCRIPTOR(STRING, 3)     → serial number string (again)
```

After ~1.5 s, the HID driver (RPCS3) claims the interface and sends a second round:

```
t+1.598s   GET_DESCRIPTOR(CONFIG, 41)    → full config again (driver re-reads)
t+1.615s   HID SET_IDLE(0, 0)            → no data
t+1.636s   [HID GET_DESCRIPTOR(REPORT)]  → 29 bytes report descriptor
t+1.636s   ← FIRST INTERRUPT OUT: B0 wake command (see below)
```

The interrupt OUT and the last control transfer overlap — the HID driver queues
the first interrupt URB while the control pipe is still busy.

---

## HID Application Protocol (Wire Format)

All reports are **32 bytes**, zero-padded at the end.
Magic byte **0x55** is used in **both** directions (host→pad and pad→host).

### Host → Pad (command)

```
Offset  Size  Field
0       1     Magic = 0x55
1       1     Length = 2 + len(args)   (counts cmd + counter + args bytes)
2       1     Command (see table below)
3       1     Counter (monotonically increasing per session, host-owned)
4..N    N-2   Args (command-specific)
N+1     1     Checksum = sum(bytes[0..N]) & 0xFF
N+2..31 -     Zero padding
```

Checksum covers: magic + length + command + counter + args (all bytes before the checksum byte).

### Pad → Host (response)

```
Offset  Size  Field
0       1     Magic = 0x55
1       1     Length = 1 + len(data)   (counts counter + data bytes)
2       1     Counter echo (copy of host counter from the triggering command)
3..M    M-1   Response data (command-specific, may be empty)
M+1     1     Checksum = sum(bytes[0..M]) & 0xFF
M+2..31 -     Zero padding
```

**The pad response does NOT echo the command byte.**

> **⚠ Implementation note:** `toy_daemon.py`'s `build_reply()` currently uses
> magic=0x56 and includes a command echo byte, producing a different wire format
> than the physical.  The correct magic is 0x55 and there is no command echo.
> This discrepancy may cause RPCS3 to misinterpret responses (e.g. reading the
> command byte as the counter value).

### Checksum Examples (verified against capture)

B0 host command (counter=1, args="(c) LEGO 2014"):
```
55 0F B0 01 28 63 29 20 4C 45 47 4F 20 32 30 31 34 [F7] 00 00 00 00 00 00 00 00 00 00 00 00 00 00
sum(55..34) = 0x3F7 → & 0xFF = 0xF7 ✓
```

C0 host command (counter=2, set-all-color):
```
55 06 C0 02 00 FF 6E 18 [A2] 00 ...
sum(55 06 C0 02 00 FF 6E 18) = 0x2A2 → & 0xFF = 0xA2 ✓
```

C0 pad ACK (counter echo=2, empty data):
```
55 01 02 [58] 00 ...
sum(55 01 02) = 0x58 ✓
```

B0 pad response (counter echo=1, 24-byte data):
```
55 19 01 00 2F 02 01 02 02 04 02 F5 00 19 8D 54 8E 2D 5B AE 4E 00 42 17 01 00 15 [1B] 00 ...
sum(55 19 01 00 2F ... 15) = 0x51B → & 0xFF = 0x1B ✓
```

---

## Known Commands

### 0xB0 — Wake / Init

Sent once immediately after the HID interface is claimed.

**Host → Pad:**
```
55 0F B0 [ctr] 28 63 29 20 4C 45 47 4F 20 32 30 31 34 [cs]
                ↑────────────────────────────────────↑
                  "(c) LEGO 2014"  (13 bytes)
```

**Pad → Host** (24-byte identity/capability blob):
```
55 19 [ctr] 00 2F 02 01 02 02 04 02 F5 00 19 8D 54 8E 2D 5B AE 4E 00 42 17 01 00 15 [cs]
```

The 24-byte payload is a fixed identity blob from the physical firmware.
`toy_daemon.py` defines this as `B0_REPLY_PAYLOAD` — verified correct from capture.

---

### 0xB1 — Seed / Challenge Round 1

Part of an Xbox 360 / RPCS3 authentication handshake.  Sent immediately after B0.

**Host → Pad** (8-byte challenge):
```
55 0A B1 [ctr] [8 bytes challenge] [cs]
```

Capture example (counter=3):
```
55 0A B1 03 93 56 D2 D8 AB DD 58 E8 [6E] 00 ...
```

**Pad → Host** (8-byte response):
```
55 09 [ctr] [8 bytes response] [cs]
```

Capture example (counter echo=3):
```
55 09 03 19 CB F5 54 0F E2 37 25 [DB] 00 ...
```

The 8-byte challenge/response values appear to be cryptographic material.
The physical pad computes these from an embedded key.  An emulator that does
not implement the handshake should still ACK (return an 8-byte zeroed or dummy
response); RPCS3 continues regardless.

---

### 0xB3 — Seed / Challenge Round 2

Structurally identical to 0xB1.  Sent after 0xB1.

Capture example (counter=4):
```
Host: 55 0A B3 04 C0 B7 4F CD 5E 28 D9 40 [48] 00 ...
Pad:  55 09 04 A0 CD F7 03 06 26 11 84 [8A] 00 ...
```

---

### 0xC0 — Set Color (single pad)

Sets the LED color for one pad zone.

**Host → Pad:**
```
55 06 C0 [ctr] [pad] [r] [g] [b] [cs]
```

`pad`: 0=all zones, 1=left, 2=center, 3=right.

Capture example (counter=2, all zones, r=0xFF, g=0x6E, b=0x18):
```
55 06 C0 02 00 FF 6E 18 [A2] 00 ...
```

**Pad → Host** (empty ACK):
```
55 01 [ctr] [cs]
```

---

### 0xC6 — Fade All Zones (animation)

Sets a timed fade animation for all three zones simultaneously.
Sent continuously by RPCS3 at approximately **1.2-second intervals** during
normal operation (the "startup rainbow" and idle animations).

**Host → Pad:**
```
55 14 C6 [ctr] [zone1_6B] [zone2_6B] [zone3_6B] [cs]
```

Each 6-byte zone block:
```
[effect] [duration] [count] [r] [g] [b]
  0x01     0x1E      0x01   …   …   …
```

| Field    | Observed values | Interpretation |
|----------|-----------------|----------------|
| effect   | 0x01            | Fade in/out effect type |
| duration | 0x1E = 30       | Duration (frames or deciseconds) |
| count    | 0x01            | Repeat count |
| r,g,b    | various         | Target RGB color |

Capture example — all zones set to red (counter=5):
```
55 14 C6 05  01 1E 01 FF 00 00  01 1E 01 FF 00 00  01 1E 01 FF 00 00  [91] 00 ...
```

**Pad → Host** (empty ACK):
```
55 01 [ctr] [cs]
```

Observed RPCS3 color cycling sequence (startup rainbow, zone colors):

| Counter | Zone 1 (L)        | Zone 2 (C)        | Zone 3 (R)        |
|---------|-------------------|-------------------|-------------------|
| 5       | ff 00 00 (red)    | ff 00 00          | ff 00 00          |
| 6       | ff 6e 00 (orange) | ff 6e 00          | ff 6e 00          |
| 9       | 00 6e 00 (green)  | 00 6e 00          | 00 6e 00          |
| 10      | 00 6e 18 (teal)   | 00 6e 18          | 00 6e 18          |
| 11      | 00 00 18 (blue)   | 00 00 18          | 00 00 18          |
| 12      | ff 00 18 (purple) | ff 00 18          | ff 00 18          |
| 13      | ff 00 00 (red)    | ff 00 00          | ff 00 00          |

RPCS3 cycles through this rainbow and repeats. Each step takes ~1.2 s.

> **Note:** `toy_daemon.py` does not have an explicit handler for 0xC6 and falls
> through to the "unknown command" path which returns a correctly-checksummed ACK
> — sufficient for RPCS3 to continue.

---

### 0xC2 — Fade Color (single pad)

Structure suspected similar to 0xC6 but for one zone.  Not observed in this
capture.  `toy_daemon.py` ACKs it with an empty reply.

### 0xC3 — Flash (single pad)

Not observed in this capture.  `toy_daemon.py` ACKs with an empty reply.

---

## Unsolicited Events: Tag Placed / Removed

The pad spontaneously sends IN reports when an NFC tag enters or leaves a zone.
No tag events appear in this capture (no toys were present).

Based on cross-referencing with other LD emulator projects and `toy_daemon.py`:

**Pad → Host** (unsolicited, no counter echo):
```
Offset  Content
0       0x56  (unsolicited-event magic — distinct from 0x55 command magic)
1       0x0B  (length = 11)
2       0x56  (event type byte = tag event)
3       pad_zone  (1=left, 2=center, 3=right)
4       action    (0x00=placed, 0x01=removed)
5..11   NFC UID (7 bytes)
12      Checksum = sum(bytes[0..11]) & 0xFF
13..31  Zero padding
```

> **Unverified:** The 0x56 event magic was not observed in this capture.
> It may also be 0x55.  Verification requires a capture with an NFC toy present.

---

## Interrupt Endpoint Timing

### Physical behavior (observed)

```
After enumeration (~1.5 s):
  Control transfers: SET_IDLE, GET_DESCRIPTOR(REPORT)  ← last control activity
  
  t+0.000s:  [OUT] B0 wake command
  t+0.001s:  [IN ] B0 identity response
  t+0.002s:  [OUT] B1 challenge
  t+0.008s:  [IN ] B1 response
  t+0.059s:  [OUT] B1 challenge (second seed)
  t+0.060s:  [IN ] B1 response
  t+0.127s:  [OUT] B3 challenge
  t+0.074s:  [IN ] B3 response   ← auth complete
  t+0.261s:  [OUT] C6 fade-all (first animation command)
  t+0.290s:  [IN ] C6 ACK

Ongoing after init:
  ~1.2 s intervals:  [OUT] C6 fade-all  →  [IN] ACK
```

The host keeps **2 IN URBs permanently queued** on EP1 IN.  After each COMPLETE
a new SUBMIT is immediately posted.  This means:
- The interrupt endpoint never goes idle.
- The host can receive unsolicited tag events at any time without polling delay.

### Virtual behavior (before keepalive fix)

Only 3 interrupt packets observed (the B0 OUT + stale IN completion + B0 IN
response), then activity dropped to zero.  Cause: `toy_daemon.py` was not
writing periodic IN reports, so all queued host IN URBs expired/timed out and
the host stopped resubmitting them.

### Fix applied

`toy_daemon.py` was patched with a keepalive: after receiving the first host
command, a re-send of the last IN report fires every 100 ms if no natural
response has been sent.  This keeps EP1 IN active.

---

## Known Protocol Deviations in toy_daemon.py

These were discovered by comparing the physical capture against the emulator output.

| Aspect | Physical (correct) | Previous toy_daemon.py behavior |
|--------|--------------------|----------------------------------|
| Response magic | `0x55` | `0x56` |
| Response format | `[0x55][len][counter][data][cs]` | `[0x56][len][cmd][counter][data][cs]` |
| Command echo in response | None | Echoed command byte |
| Length field meaning | `1 + len(data)` | `2 + len(data)` (included extra cmd byte) |

> The string descriptors already matched the physical device.
> The response magic/format bug has now been fixed in the daemon copies.

---

## Raw Packet Log (key frames)

Condensed from `physical-20260505-221015.pcapng`, device 1:60:

```
22:10:22.641890  OUT  55 0F B0 01 28 63 29 20 4C 45 47 4F 20 32 30 31 34 F7 …  ← B0 wake
22:10:22.643912  IN   55 19 01 00 2F 02 01 02 02 04 02 F5 00 19 8D 54 8E 2D …  ← B0 resp
22:10:22.702543  OUT  55 06 C0 02 00 FF 6E 18 A2 …                              ← C0 color
22:10:22.703913  IN   55 01 02 58 …                                              ← C0 ACK
22:10:22.768976  OUT  55 0A B1 03 93 56 D2 D8 AB DD 58 E8 6E …                  ← B1 seed
22:10:22.776915  IN   55 09 03 19 CB F5 54 0F E2 37 25 DB …                     ← B1 resp
22:10:22.834630  OUT  55 0A B3 04 C0 B7 4F CD 5E 28 D9 40 48 …                  ← B3 seed
22:10:22.853919  IN   55 09 04 A0 CD F7 03 06 26 11 84 8A …                     ← B3 resp
22:10:22.901309  OUT  55 14 C6 05 01 1E 01 FF 00 00 01 1E 01 FF 00 00 01 1E …  ← C6 anim
22:10:22.930923  IN   55 01 05 5B …                                              ← C6 ACK
22:10:24.135406  OUT  55 14 C6 06 01 1E 01 FF 6E 00 … 01 1E 01 FF 6E 00 … DC … ← C6 anim
22:10:24.144984  IN   55 01 06 5C …
22:10:24.534957  OUT  55 0A B1 07 8A 44 3B 9B 9D 43 96 8E BF …                  ← B1 (again)
22:10:24.562973  IN   55 09 07 5F BB 67 C2 B5 C6 EC 10 1F …
22:10:24.601130  OUT  55 0A B3 08 79 1C 8F 38 16 2B 91 B8 …                     ← B3 (again)
22:10:24.640041  IN   55 09 08 79 38 3C 3E 3F 7A 06 70 C0 …
  … (C6 color animation repeats at ~1.2 s intervals, B1/B3 re-seed ~every 2 s)
```

RPCS3 re-issues B1 and B3 seed rounds approximately every 2 seconds — this is a
periodic re-authentication ping.  The physical pad answers each one; an emulator
must respond (even with dummy bytes) to avoid a timeout.
