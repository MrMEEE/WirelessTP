# WirelessTP Communication Architecture

## System topology

```
Physical Toy Pad (USB HID, VID:PID 0e6f:0241)
        │
        │ USB OTG – host mode (pad-esp32 is host)
        │ 32-byte interrupt IN/OUT reports @ 1ms poll
        ▼
┌────────────────────────┐
│  ESP32-S2 Mini         │  pad-esp32
│  USB host + WiFi STA   │
│  GPIO43=TX  GPIO44=RX  │  (UART for debug only – no LP traffic)
└────────────────────────┘
        │
        │ WiFi – STA joins console softAP
        │ TCP socket, port 25100, LP frames (Link A)
        │ WiFi.setSleep(false), setNoDelay(true)
        ▼
┌────────────────────────┐
│  ESP32 (dual-core)     │  console-esp32
│  WiFi softAP           │  IP 192.168.44.1 (DHCP)
│  GPIO16=RX  GPIO17=TX  │  UART to RP2040 (Serial2, 115 200 baud)
│  USB CDC for debug     │
└────────────────────────┘
        │
        │ UART 115 200 baud, LP frames (Link B)
        │ RP2040 Serial1 D6=TX D7=RX ↔ ESP32 Serial2 GPIO17=TX GPIO16=RX
        ▼
┌────────────────────────┐
│  XIAO RP2040           │  console-rp2040
│  USB HID device        │  emulates Toy Pad
│  (bare-metal loop)     │
└────────────────────────┘
        │
        │ USB HID, VID 0e6f PID 0241 ("LEGO READER V2.10")
        │ 32-byte interrupt IN/OUT reports @ 1ms poll
        ▼
     PS3 / RPCS3 (game)
```

---

## Link A — pad-esp32 ↔ console-esp32 (WiFi TCP)

### Transport

| Property | Value |
|----------|-------|
| Transport | TCP (not UDP — the original spec was provisional) |
| Console port | 25100 |
| Console IP | 192.168.44.1 (softAP gateway) |
| WiFi AP SSID | `ToyPadConsole-{CHIPID_24LSB_HEX}` |
| AP security | Open (no password) |
| Modem sleep | Disabled: `WiFi.setSleep(false)` on pad |
| Nagle | Disabled: `setNoDelay(true)` on both ends after connect |
| Frame format | LP (see protocol spec) |

### Pad connection state machine

```
CONN_SEARCHING
  ├─ Blink LED yellow (400 ms on/off)
  ├─ TCP connect attempt every kRetryMs=50ms (after disconnect)
  ├─ Send HELLO every kHelloMs=2000ms once connected
  └─► CONN_BLINK_OK   on PAIR_SET received
      └─► CONN_PAIRED  after 3× green blink (≈1200 ms)
CONN_PAIRED
  ├─ Send HELLO every kHeartbeatMs=5000ms (trackAck=true)
  ├─ Re-sync state every kStateSyncMs=1000ms
  ├─ Send pad-dbg (WiFi RSSI/status) every 1000ms
  ├─► CONN_SEARCHING  if !client.connected()  [fast path, no blink]
  └─► CONN_BLINK_FAIL if lastPeerMs > kLostMs=30000ms
CONN_BLINK_FAIL
  ├─ Blink LED red 3×
  └─► CONN_SEARCHING
```

### Pairing sequence (first connection)

```
pad-esp32                            console-esp32
    │                                     │
    │── TCP connect ──────────────────────►│ (accept, setNoDelay, lp_stream_reset)
    │                                     │
    │── HELLO [0xa1]  (trackAck=true) ───►│ isValidPadHello: length=1 → enrollment
    │                                     │ ensureSecret() → generates random 32-bit secret
    │◄── PAIR_SET [secret_u32_le] ────────│
    │                                     │
    │── ACK [pair_set_seq] ───────────────►│
    │  (pending.active=false, paired=true) │
    │  saveSharedSecret()                  │
    │  connState = CONN_BLINK_OK           │
```

### Subsequent connections (known pad)

```
pad-esp32                            console-esp32
    │                                     │
    │── TCP connect ──────────────────────►│
    │── HELLO [0xa1, secret_u32_le] ──────►│ secret matches → padPaired=true
    │◄── PAIR_SET [same secret] ───────────│
    │── ACK ──────────────────────────────►│
```

### Frame flows while paired

| Direction | Frame | Sender action | Receiver action |
|-----------|-------|--------------|-----------------|
| pad→console | HELLO | every 5 s heartbeat, trackAck=true | ACK, no forward to RP2040 |
| pad→console | TAG_SET | figure placed (physical or virtual), trackAck=true for real events | ACK, forward to RP2040 UART |
| pad→console | TAG_CLEAR | figure removed, trackAck=true | ACK, forward to RP2040 UART |
| pad→console | ACK | response to console LED_CMD | forward to RP2040 UART (harmless; RP2040 does `break`) |
| pad→console | DEBUG (0x40) | WiFi diagnostics every 1 s | print `[pad-dbg]`, **not** forwarded to RP2040 |
| console→pad | LED_CMD | state sync every 1 s (from `lightZones[]`) | ACK, apply colour to physical pad |
| console→pad | PAIR_SET | on pairing | pad saves secret, enters BLINK_OK |

### Pad periodic state sync (every kStateSyncMs=1000ms in CONN_PAIRED)

- Sends TAG_SET for each occupied physical USB zone (zones 0-2 → slots 1-3), trackAck=false
- Sends TAG_SET for each occupied virtual slot (4-7, or 1-7 in no-USB builds), trackAck=false
- Does **not** send TAG_CLEAR for empty slots (avoids queue floods)

### Pad diagnostic frame (LP_MSG_DEBUG)

Sent every 1 second while in CONN_PAIRED:

```
payload: "rssi=-47 wf=3"   (rssi = WiFi.RSSI(), wf = WiFi.status())
```

Console prints as `[pad-dbg] rssi=-47 wf=3` to USB serial. Never forwarded to RP2040.

---

## Link B — console-esp32 ↔ console-rp2040 (UART)

### Transport

| Property | Value |
|----------|-------|
| Physical | UART, 115 200 baud, 8N1 |
| Console ESP32 | Serial2, GPIO16=RX / GPIO17=TX |
| RP2040 | Serial1, D6=TX / D7=RX |
| Frame format | LP (same protocol as Link A) |

### HELLO heartbeats (bidirectional, independent timers)

| Sender | Payload | Interval | Purpose |
|--------|---------|----------|---------|
| console-esp32 → RP2040 | `[0xb1]` | 3 000 ms | console alive signal |
| RP2040 → console-esp32 | `[0x02]` | 4 000 ms | RP2040 alive signal |

Both sides ACK received HELLOs; neither side takes action on missed heartbeats (no timeout logic implemented).

### Frame forwarding rules (console-esp32)

**From pad TCP → RP2040 UART:**

| Frame type | Action |
|-----------|--------|
| HELLO | Drop (pad↔console specific) |
| PAIR_SET | Drop (pad↔console specific) |
| DEBUG (0x40) | Print `[pad-dbg]`, drop — NOT forwarded |
| ACK (0x7F) | Currently forwarded (RP2040 ignores with `case LP_MSG_ACK: break`) |
| TAG_SET, TAG_CLEAR | Re-encode with original seq and write to Serial2 |

**From RP2040 UART → pad TCP:**

| Frame type | Action |
|-----------|--------|
| LED_CMD | Forward to pad TCP via `sendFrameTcp()` |
| HELLO, ACK, DEBUG, all others | Drop |

**ACK sending:**
- All non-ACK frames from pad TCP → ACK sent back to pad TCP
- All non-ACK / non-DEBUG frames from RP2040 UART → ACK sent back to RP2040 UART

### Console state sync (every kStateSyncMs=1000ms)

- TAG_SET to RP2040 UART for each occupied slot in `padSlots[]`
- LED_CMD to pad TCP for each zone in `lightZones[]` (only if `padPaired`)
- `padSlots[]` is **not** cleared when the pad disconnects

### RP2040 toy event queue

- `enqueueToyTagEvent()` queues a 32-byte USB IN packet (placed/removed event)
- `serviceToyInQueue()` drains the queue each loop: peek → `usb_hid.sendReport()` → pop on success
- If `!TinyUSBDevice.mounted()`: entire queue is discarded (`memset(&toyInQueue, 0, ...)`) to prevent stale events accumulating

---

## USB HID protocol (RP2040 ↔ game)

### Descriptors

```
VID:PID      0x0e6f : 0x0241
Manufacturer "PDP LIMITED. "
Product      "LEGO READER V2.10"
Serial       "P.D.P.000000"
Class        HID (vendor-specific usage page 0xff00)
bmAttributes 0x80  (bus powered)
bMaxPower    250   (500 mA)
Report size  32 bytes IN + 32 bytes OUT @ 1 ms poll
bcdHID       0x0100 (patched from TinyUSB default 0x0110)
EP packet    32 bytes IN / 32 bytes OUT (patched from 64)
```

### Host→Pad OUT packets (command)

```
[0] 0x55  magic
[1] len   data bytes following (= 1 + argsLen + 1 for counter+args+checksum)
[2] cmd
[3] counter
[4..4+argsLen-1] args
[4+argsLen] checksum = sum(bytes[0..4+argsLen-1]) & 0xff
[...] zero padding to 32 bytes
```

### Pad→Host IN packets

**Command reply:**
```
[0] 0x55  magic
[1] len   (= 1 + payloadLen)
[2] counter (echoes command counter)
[3..3+payloadLen-1] reply payload
[3+payloadLen] checksum
[...] zeros
```

**Tag event:**
```
[0]  0x56  magic
[1]  0x0b  payload length
[2]  pad   (1=center, 2=left, 3=right)
[3]  0x00  reserved
[4]  figIndex (= slot number 1-7, unique per toy)
[5]  action (0=placed, 1=removed)
[6..12] uid (7 bytes)
[13] checksum
[...] zeros
```

### Commands

| Cmd | Name | Args | Reply payload |
|-----|------|------|---------------|
| B0 | Init | startup string (observed 16 bytes) | 30-byte capability response |
| B1 | RNG seed | 8 bytes (TEA-encrypted seed+confirmation) | 8 bytes (TEA-encrypted confirmation) |
| B3 | RNG challenge | 8 bytes (TEA-encrypted confirmation) | 8 bytes (TEA-encrypted nextRand+confirmation) |
| C0 | LED immediate | pad(1-3/0=all), r, g, b | empty |
| C2 | LED fade | pad, pulseTime, pulseCount, r, g, b | empty |
| C3 | LED flash | pad, onLen, offLen, pulseCount, r, g, b | empty |
| C6 | LED tri-fade | 3×6 bytes (center, left, right): effect, dur, cnt, r, g, b | empty |
| C8 | LED tri-set | 3×4 bytes (center, left, right): enable, r, g, b | empty |
| D2 | NFC page read | figIndex, startPage → 4 pages (16 bytes) | 0x00 + 16 bytes |
| D3 | NFC page write | figIndex, page, 4 bytes | empty |
| D4 | NFC extended | figIndex, page, ... | varies |

### B1/B3 cryptography

Algorithm matches RPCS3 `Dimensions.cpp`:
- TEA (Tiny Encryption Algorithm), 32 rounds, key = `kCommandKey` (16 bytes hard-coded)
- B1: decrypt challenge → extract seed → init Bob Jenkins PRNG → encrypt confirmation
- B3: decrypt challenge → `rngGetNext()` → encrypt [nextRand LE | conf BE]

### NFC figure data synthesis (D2 replies)

Toy ID is 32 bits: `uid[0..3]` (LE) for physical figures, or the virtual `toyId` for emulated ones.

UID is synthesised as:
```
uid[0..3] = toyId (LE)
uid[4]    = 0xa5
uid[5]    = slot (1-7)   ← makes each slot's UID unique
uid[6]    = toyId ^ 0x5a
```

Pages 0, 1, 2 contain the NTAG213 UID/BCC structure.
Pages 36-37 contain the TEA-encrypted figure identity, using a per-UID figure key
generated by `generateFigureKey()` (matching RPCS3's `generate_figure_key`).

---

## Zone / slot / pad-index convention

Three coordinate systems exist; mappings are defined in firmware:

| Name | Values | Used by |
|------|--------|---------|
| LP zone | 0=center, 1=left, 2=right | Link A+B LP frames |
| Toy Pad pad-index | 1=center, 2=left, 3=right | USB HID C0/C2/C3 commands |
| Web UI slot | 1-7 | console-esp32 web API, LP TAG_SET/CLEAR payload byte 0 |
| figIndex | 1-7 (= slot) | USB IN tag events, D2 args |

**Slot-to-zone mapping (mirrored in ESP32 and RP2040):**

| Slot | LP zone | Pad-index |
|------|---------|-----------|
| 1 | 1 (left) | 2 |
| 2 | 0 (center) | 1 |
| 3 | 2 (right) | 3 |
| 4 | 1 (left) | 2 |
| 5 | 1 (left) | 2 |
| 6 | 2 (right) | 3 |
| 7 | 2 (right) | 3 |

**Physical USB zone → LP slot (pad-esp32, kZoneToSlot):**

| USB zone (from toypad IN report) | LP zone | Slot |
|----------------------------------|---------|------|
| zone byte 1 (center) | 0 | 2 |
| zone byte 2 (left) | 1 | 1 |
| zone byte 3 (right) | 2 | 3 |

---

## Timing reference

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `kHelloMs` | 2 000 ms | pad-esp32 | HELLO rate while searching |
| `kPairTimeoutMs` | 15 000 ms | pad-esp32 | Give up pairing → blink fail |
| `kHeartbeatMs` | 5 000 ms | pad-esp32 | HELLO rate while paired |
| `kLostMs` | 30 000 ms | pad-esp32 | Console silent → blink fail |
| `kStateSyncMs` | 1 000 ms | pad-esp32 | State re-broadcast interval |
| `kRetryMs` | 50 ms | pad-esp32 | ACK retry interval |
| `kMaxRetries` | 3 | pad-esp32 | Max retry count before drop |
| `kBlinkOnMs/OffMs` | 200/200 ms | pad-esp32 | OK/fail blink timing |
| `kHelloMs` | 3 000 ms | console-esp32 | Console→RP2040 heartbeat rate |
| `kStateSyncMs` | 1 000 ms | console-esp32 | State re-broadcast to RP2040+pad |
| `kHelloMs` | 4 000 ms | console-rp2040 | RP2040→console heartbeat rate |

---

## Reliability model

### Pad-side (Link A outbound)

`sendFrame(type, payload, len, trackAck)`:
- `trackAck=true`: stores frame in `pending`; retried every `kRetryMs=50ms` up to `kMaxRetries=3` from the CONN_PAIRED loop
- `trackAck=false`: fire-and-forget

Frames sent with `trackAck=true`:
- HELLO (heartbeat and searching)
- TAG_SET / TAG_CLEAR from physical USB host events
- TAG_SET / TAG_CLEAR from virtual toy menu

Frames sent `trackAck=false` (fire-and-forget):
- State sync TAG_SET (periodic re-broadcast — retrying is redundant with periodic sync)
- ACK responses
- DEBUG diagnostics

### Console-side (Links A+B inbound)

Every received non-ACK frame generates an ACK back to the sender (pad TCP or RP2040 UART).
The console itself does not have a retry mechanism for outbound frames — it relies on the
periodic state sync to recover from dropped LED_CMDs and TAG_SETs.

---

## Provisioning flow (pad-esp32 first run)

1. Pad has no WiFi credentials in NVS → enters provisioning AP mode
2. Provisioning AP SSID: `ToyPad-Setup`, password: `toypadsetup`
3. DNS catches all hostnames (captive portal pattern)
4. User visits `http://192.168.x.x/` → selects console AP from scan list → submits
5. Credentials saved to NVS (`toypad` namespace, keys `ssid`/`pass`)
6. Reboot → connects to console AP → proceeds to LP pairing

Factory reset: hold BOOT button (GPIO 0) for 3 s → NVS clear + reboot.

---

## Debug interfaces

### pad-esp32

| Interface | Details |
|-----------|---------|
| UART debug | GPIO43=TX / GPIO44=RX, 115 200 baud (USB CDC disabled in USB-host builds) |
| Serial menu | Interactive: press any key; commands `s` / `p <slot>` / `r <slot>` / `f <text>` |
| Web UI | `http://<pad-IP>/` — shows state, console IP, WiFi info |
| LED feedback | Yellow=searching, green blink=paired, red blink=fail |
| pad-dbg relay | WiFi RSSI + status relayed to console as LP_MSG_DEBUG every 1 s |

### console-esp32

| Interface | Details |
|-----------|---------|
| USB CDC serial | 115 200 baud; also prints RP2040 debug (`[lp-debug]`) and pad debug (`[pad-dbg]`) |
| Serial commands | `help`, `pi-console on/off` |
| Web UI | `http://192.168.44.1/` — full toy pad management UI (slots, toybox, LED state, catalog) |
| Log prefixes | `[console-esp32]` events, `[led-rx]` LED commands, `[lp-debug]` RP2040 debug, `[pad-dbg]` pad debug |

### console-rp2040

| Interface | Details |
|-----------|---------|
| UART debug | Serial1 shared with LP link (debug output interleaved with LP frames — console-esp32 filters by CRC) |
| LP_MSG_DEBUG | Sent via UART; console-esp32 prints as `[lp-debug]` |
| Stats | `b0 stats rx=%u q=%u tx=%u` printed every 3 000 ms |

---

## Known issues and active investigations

### TCP disconnect every 3-5 seconds (active)

The pad TCP connection drops reliably every few seconds. WiFi is confirmed stable
(wf=3, rssi=-43 to -47 throughout). The `WiFiClient::connected()` implementation
calls `recv(fd, 0, MSG_DONTWAIT)` and returns false only on RST-level errors
(ECONNRESET/EPIPE/EBADF); FIN does not trigger it. The source of the RST is unknown.

Leading hypotheses (in priority order):
1. lwIP internal socket management on the ESP32-S2 — possibly a timer or PCB limit
2. The console forwarding LP_MSG_ACK frames (from pad's LED_CMD ACKs) to RP2040 UART; RP2040 ignores them but the UART traffic is unnecessary
3. The console's `padClient = incoming` path — replacing a connected client when a new TCP SYN arrives (would print "replacing stale" but may happen silently if `padClient.connected()` transiently returns false)

To diagnose: add errno capture in `tcpSendWire()` and a raw `recv()` probe in
the CONN_PAIRED disconnect handler; relay as first pad-dbg of next connection.

### LP_MSG_ACK forwarding to RP2040 (minor)

The console currently forwards pad ACKs (0x7F) to the RP2040 via UART. The RP2040
handles these with `case LP_MSG_ACK: break;` — safe but wasteful. These 3 frames/second
(one per LED_CMD zone) serve no purpose at the RP2040 level and should be filtered.
