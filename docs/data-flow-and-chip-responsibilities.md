# Data Flow and Chip Responsibilities

## System Overview

```
Physical Toypad ──USB HID──► pad-esp32 ──WiFi TCP──► console-esp32 ──UART──► console-rp2040 ──USB HID──► Game (RPCS3/PS3)
                 ◄──USB HID──             ◄──WiFi TCP──               ◄──UART──               ◄──USB HID──
                  (LED cmds)               (LED cmds)                   (LED cmds)
```

---

## 1. Physical Toypad → pad-esp32 (USB HID IN, 32-byte reports)

### Tag event (toy placed or removed)

| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0x56` | Magic (portal → host event) |
| 1 | `0x0b` | Payload length |
| 2 | 1–3 | Physical pad zone: 1=center, 2=left, 3=right |
| 3 | `0x00` | Reserved |
| 4 | `0x00` | Reserved |
| 5 | 0 / 1 | Action: 0=placed, 1=removed |
| 6–12 | — | NFC UID (7 bytes, NTAG213, BCC0 excluded) |
| 13 | — | Checksum (sum of bytes 0–12, mod 256) |

Pad-esp32 converts `padZone` (1-3) to `lpZone` (0-2): `lpZone = padZone - 1`.

### D2 NFC page-read reply (success)

| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0x55` | Magic |
| 1 | `0x12` | Reply type |
| 2 | — | Counter (echoed from the D2 request) |
| 3 | `0x00` | Status: 0 = success |
| 4–19 | — | 16 bytes of NFC data (4 pages starting from the requested page) |

### D2 NFC page-read reply (error)

Byte 3 is non-zero, or length field is unexpected. Pad-esp32 aborts and
retries the slot (up to 3 attempts on the hint zone, once on fallback zones).

---

## 2. pad-esp32 → Physical Toypad (USB HID OUT, 32-byte reports)

All out-reports: `[0x55, len, cmd, counter, args..., checksum, 0x00 padding]`.
Checksum = sum of bytes [0..(len+1)], mod 256.

| Command | Bytes | Description |
|---------|-------|-------------|
| B0 wake | `55 0f b0 01 "(c) LEGO 2014" checksum` | Sent on device open to initialise pad |
| B1 seed | `55 0a b1 ctr TEA(seed_LE\|conf_BE)[8] checksum` | Seeds pad RNG |
| B3 challenge | `55 0a b3 ctr TEA(conf_BE\|00000000)[8] checksum` | Completes auth |
| C0 LED | `55 06 c0 ctr padIdx r g b checksum` | padIdx: 0=all, 1=center, 2=left, 3=right |
| D2 read | `55 04 d2 ctr figIdx page checksum` | figIdx: 0=center, 1=left, 2=right; page: NFC page number |

---

## 3. pad-esp32 NFC Figure Key Derivation

Used only by pad-esp32 to read the LEGO character/vehicle ID from physical NFC
page 36.  The algorithm is a port of RPCS3 `Dimensions.cpp`.

**`padFigScramble(uid[7], count)`**
1. Build a 24-byte buffer: `uid[7] ++ kCharConstant[17]`.
2. Set `buf[count * 4 - 1] = 0xaa` — the Ellerbach constant used by the physical
   toypad's firmware to scramble factory-written character tags.
   (RPCS3's emulator uses `^= count` instead; vehicles written by the game use
   that algorithm — see the vehicle note below.)
3. Run `dimensions_randomize()` over the buffer in 4-byte little-endian chunks.

**`padGenerateFigureKey(uid[7], key[16])`**
1. Compute `s3..s6 = padFigScramble(uid, 3..6)`.
2. Store each `u32` as **big-endian** into 4 consecutive key bytes.
   The physical toy's NFC was encrypted with big-endian key words (matching
   RPCS3's `write_to_ptr<be_t<u32>>`).  The TEA decrypt then reads those bytes
   back as little-endian, which is equivalent to `byteswap(sN)` as the TEA key
   word — exactly what the physical toy was encrypted with.

**`padTeaDecryptWithKey(in[8], out[8], key[16])`**
Standard 32-round Tiny Encryption Algorithm (TEA) decrypt, reading key words as
little-endian from the 16-byte key buffer.

**Validation:** `readPhysicalToyId` decrypts NFC page 36 → 8 bytes → two u32
words `id0` and `id1`.  The encoding stores the character ID twice; `id0 == id1`
confirms the correct key was used and the correct reader slot was queried.

### Vehicle note

Vehicles start blank and are programmed by the game console at runtime.  The game
writes NFC pages using the RPCS3 scramble variant (`^= count`, not `= 0xaa`).
Reading vehicle IDs from physical tags therefore requires a different scramble
constant.  This is tracked as a separate work item.

---

## 4. pad-esp32 → console-esp32 (WiFi TCP, LP framing)

LP frame: `[L D 01 type seq len] [payload…] [crc16 2 bytes]`

| Message | Type | Payload | Meaning |
|---------|------|---------|---------|
| `TAG_SET` | `0x10` | `[slot(1-7), zone(0-2), toyId[4 LE]]` | Physical toy placed; includes resolved LEGO character ID |
| `TAG_CLEAR` | `0x11` | `[slot]` | Physical toy removed |
| `HELLO` | `0x01` | `[0xa1, secret[4 LE]?]` | Heartbeat / pairing |
| `ACK` | `0x7f` | `[echoed-seq]` | Acknowledged a reliable frame |
| `DEBUG` | `0x40` | ASCII string | D2 result log (e.g. `d2:ok:0001 fi=0`) |

Zone encoding: `0` = center, `1` = left, `2` = right (LP convention throughout).

---

## 5. console-esp32 Responsibilities

- Runs a **TCP server** on port 25100, accepts the pad-esp32 connection.
- Runs a **WiFi AP** (or STA) so the pad-esp32 can reach it.
- Maintains **`padSlots[7]`**: slot occupancy, toyId, display label, type.
- Maintains **`lightZones[3]`**: per-zone LED RGB state.
- Forwards `TAG_SET` / `TAG_CLEAR` and `LED_CMD` between pad-esp32 (TCP) and
  console-rp2040 (UART) in both directions.
- Serves the **main web UI** on port 80 — shows game slots, toybox, LED colours.

---

## 6. console-esp32 → console-rp2040 (UART 115200, LP framing)

Same LP framing as TCP.  Messages forwarded:

| Message | Direction | Payload |
|---------|-----------|---------|
| `TAG_SET` | ESP32 → RP2040 | `[slot, zone, toyId[4 LE]]` |
| `TAG_CLEAR` | ESP32 → RP2040 | `[slot]` |
| `LED_CMD` | RP2040 → ESP32 | `[zone(0-2 or 0xff=all), r, g, b]` |
| `HELLO` | Both | Heartbeat |

---

## 7. console-rp2040 Responsibilities

- Acts as a **USB HID device** to the game console, presenting as the LEGO
  Dimensions Toy Pad (VID `0x0e6f`, PID `0x0241`, "LEGO READER V2.10").
- Maintains **`ToyPadState`**: per-zone LED colour, per-slot UID and toyId.
- On `TAG_SET`: stores `toyId`, builds a synthetic 7-byte NFC UID from it, and
  encrypts `toyId` (twice, as a TEA pair) into a synthetic NFC block using
  `generateFigureKey` (RPCS3 algorithm: `^= count`, BE key storage).
- On game `B0`: queues a hardcoded startup reply matching a real pad's response.
- On game `B1` / `B3`: performs TEA auth using `kCommandKey`.
- On game `D2` (block-read): serves synthetic NFC data for the requested slot and
  page range, applying the D3 write cache for vehicle upgrade pages.
- On game `D3` (block-write): stores written pages in the write cache.
- On game `C0` (LED set): extracts RGB, updates LED state, sends `LED_CMD` over
  UART to console-esp32.

**Key difference from pad-esp32:** rp2040 `figureScramble` uses `buf[count*4-1]
^= count` (RPCS3 algorithm), while pad-esp32 `padFigScramble` uses `= 0xaa`
(Ellerbach / physical-toy algorithm).  Both store the key as big-endian.  The
rp2040 path is self-consistent with RPCS3: the game encrypts with `^= count` +
BE, and the rp2040 emulates the same.

---

## 8. console-rp2040 → Game (USB HID IN, 32-byte reports)

### Tag event

| Byte | Value | Description |
|------|-------|-------------|
| 0 | `0x56` | Magic |
| 1 | `0x0b` | Payload length |
| 2 | 1–3 | Pad zone in toy-pad convention: 1=center, 2=left, 3=right |
| 3 | `0x00` | Reserved |
| 4 | — | Figure index = slot number (1-7, stable per toy) |
| 5 | 0 / 1 | Action: 0=placed, 1=removed |
| 6–12 | — | Synthetic NFC UID (7 bytes, derived from toyId and slot) |
| 13 | — | Checksum |

`lpZone` (0-2) → toypad convention (1-3): `lpZoneToToyZone(z) = z + 1`.

### D2 NFC page-read reply

Same format as physical pad reply (bytes 0-19), but data is synthetic —
generated by `generateFigureKey` + TEA encrypt using the RPCS3 algorithm.

---

## 9. Webui Toy Name Resolution

The webui on console-esp32 (and pad-esp32) uses the compiled-in catalog
(`ld_catalog_data.h`) to resolve `toyId` → display name:

```js
function toyInfo(id) {
  return catalog[id] || { name: 'Toy ' + id + ' 0x' + id.toString(16), world: '?' };
}
```

If `readPhysicalToyId` returns the wrong `toyId` (e.g. 257 instead of 1 for
Batman), `catalog[257]` is undefined and the webui shows `"Toy 257 0x101"`.
The root cause was a key-endianness bug in `padGenerateFigureKey` (fixed —
see `architecture-decisions.md` for details).
