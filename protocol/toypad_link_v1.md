# Toy Pad Link Protocol v1

This protocol is used across two logical links:

- **Link A**: pad-esp32 ↔ console-esp32 over Wi-Fi TCP (not UDP — original spec was provisional)
- **Link B**: console-esp32 ↔ console-rp2040 over UART (115 200 baud)

Wi-Fi mode for Link A:

- Console ESP32 is a SoftAP (SSID `ToyPadConsole-{CHIPID}`, open, IP 192.168.44.1).
- Pad ESP32 joins console SoftAP as station after provisioning.
- TCP server on port 25100. `setNoDelay(true)` on both ends. `WiFi.setSleep(false)` on pad.

## Frame format

```
[0]  sync0   = 0x4C  ('L')
[1]  sync1   = 0x44  ('D')
[2]  version = 0x01
[3]  type    (see below)
[4]  seq     (per-sender, wraps at 256)
[5]  length  (payload byte count, 0..64)
[6..6+length-1]  payload
[6+length..6+length+1]  CRC16-CCITT (init=0xFFFF, poly=0x1021, big-endian)
```

Maximum wire size: 6 (header) + 64 (payload) + 2 (CRC) = 72 bytes.

## Message types

| Value | Name | Direction | Description |
|-------|------|-----------|-------------|
| 0x01 | HELLO | both | Heartbeat / pairing identity |
| 0x10 | TAG_SET | pad→console, console→RP2040 | Figure placed on a slot |
| 0x11 | TAG_CLEAR | pad→console, console→RP2040 | Figure removed from a slot |
| 0x20 | LED_CMD | RP2040→console→pad | Zone LED colour command |
| 0x30 | PAIR_SET | console→pad | Deliver shared pairing secret |
| 0x40 | DEBUG | any | Diagnostic text string (not forwarded by console) |
| 0x7F | ACK | any | Acknowledge a received frame |

## Payload formats

### HELLO (0x01)

| Sender | Length | Payload |
|--------|--------|---------|
| Pad (enrollment) | 1 | `[0xa1]` |
| Pad (authenticated) | 5 | `[0xa1, secret_u32_le[0..3]]` |
| Console→RP2040 heartbeat | 1 | `[0xb1]` |
| RP2040→Console heartbeat | 1 | `[0x02]` |

### TAG_SET (0x10)

```
[0]  slot   (1–7)
[1..4]  toyId_u32_le
```

`toyId` is the first 4 bytes of the NFC UID (little-endian) for physical figures,
or a virtual toy ID assigned by the user.

### TAG_CLEAR (0x11)

```
[0]  slot   (1–7)
```

### LED_CMD (0x20)

```
[0]  zone   (0=center, 1=left, 2=right, 0xFF=all zones)
[1]  r
[2]  g
[3]  b
```

### PAIR_SET (0x30)

```
[0..3]  secret_u32_le
```

### DEBUG (0x40)

```
[0..length-1]  UTF-8 text (not null-terminated)
```

Used by pad-esp32 to relay WiFi diagnostics to console (format: `"rssi=-47 wf=3"`).
The console prints it as `[pad-dbg] …` and does not forward it to the RP2040.

### ACK (0x7F)

```
[0]  seq of the frame being acknowledged
```

## Reliability strategy

Only the **pad-esp32** implements retry logic:

- Frames sent with `trackAck=true` are stored in a single-slot `pending` buffer.
- Retry interval: `kRetryMs=50ms`, max retries: `kMaxRetries=3`.
- If the ACK arrives (matching `pending.seq`), `pending.active=false`.
- If retries are exhausted the frame is dropped silently (link recovery relies on periodic state sync).

`trackAck=true` is used for: HELLO (both modes), physical TAG_SET/TAG_CLEAR.  
`trackAck=false` (fire-and-forget) is used for: state-sync TAG_SET re-broadcasts, ACK responses, DEBUG frames.

The **console-esp32** does not retry; it relies on the 1-second state sync to re-deliver
any lost TAG_SET or LED_CMD.

## Pairing model

1. Pad connects to console AP, opens TCP socket.
2. Pad sends `HELLO [0xa1]` (no secret) or `HELLO [0xa1, secret]` (known secret).
3. Console validates:
   - Length=1 → enrollment: generates `esp_random()` secret, saves, sends `PAIR_SET`.
   - Length=5 with matching secret → re-pair: sends `PAIR_SET` with same secret.
   - Mismatch → ignored.
4. Pad receives `PAIR_SET`, saves secret to NVS, sends ACK, enters paired state.
5. Subsequent connections skip enrollment and go straight to step 2 with the stored secret.

Console accepts any new TCP connection and replaces an existing stale one. The pad
always reconnects immediately after detecting disconnect (`client.stop()` + CONN_SEARCHING).

## Security

No crypto on the link itself. The 32-bit shared secret prevents a rogue device from
pairing without access to a prior session. Add HMAC or AES-GCM in v2 if the link
is exposed to untrusted networks.

## Notes

- The LP frame parser (`lp_stream_push`) is byte-streaming: it tolerates arbitrary
  TCP segmentation and UART framing gaps.
- `seq` wraps at 256. The retry window (3 × 50 ms) is short enough that wrap
  collision with a live pending frame is not a practical concern.
- Do not optimize packet size until end-to-end behavior is fully verified.
- Full system architecture, timing constants, zone/slot mappings, and USB HID protocol
  details are in `docs/communication-architecture.md`.
