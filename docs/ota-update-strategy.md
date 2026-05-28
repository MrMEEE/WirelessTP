# OTA / Firmware Update Strategy

## Overview

Three SoCs need to be updated when new firmware is released:

| SoC | Role | Link to console-esp32 |
|---|---|---|
| **console-esp32** | WiFi AP, web UI, TCP broker | — (self) |
| **console-rp2040** | USB HID device to PC/RPCS3 | UART (115200 baud, LP protocol) |
| **pad-esp32** | USB host for physical toypad, WiFi station | WiFi TCP (LP protocol, port 25100) |

The goal is a one-click update flow from the console-esp32 web UI that requires no physical button presses on any board.

---

## Per-Chip Strategy

### 1. console-rp2040 — Custom UART DFU (zero-touch)

The console-esp32 streams the new RP2040 firmware directly over the existing UART connection
using a lightweight chunked DFU protocol. The RP2040 stages the image in the upper half of
its 2 MB flash using arduino-pico's `Updater` class, then reboots to apply it. No button
press, no drag-and-drop — fully automatic.

**Why not BOOTSEL?**
When the RP2040 enters BOOTSEL mode it presents a USB mass-storage drive on the **PC**,
not on the UART-connected console-esp32. The plain ESP32 has no USB host hardware, so it
cannot write to that drive. BOOTSEL is not useful for an automated update flow.

**Why arduino-pico `Updater`?**
The EarlePhilhower arduino-pico fork includes an `Updater` class that writes incoming bytes
to a staging area in the upper half of flash. Only when `Updater.end()` is called and the
full-image CRC32 matches does it mark the staged firmware as the next boot target. If power
is cut at any point before the commit, the current firmware is untouched — no brick risk.

**Trigger path:**
1. Console-esp32 web UI receives `POST /ota/upload-rp2040`; binary stored in LittleFS.
2. Console-esp32 sends `LP_MSG_OTA_BEGIN` (0x50) over UART, payload `[size LE4][crc32 LE4]`.
3. RP2040 calls `Updater.begin(size)`, replies `LP_MSG_ACK`.
4. Console-esp32 streams firmware data as DFU chunks (see UART DFU Protocol section).
5. Console-esp32 sends `LP_MSG_OTA_COMMIT` (0x51) with empty payload.
6. RP2040 calls `Updater.end()`, verifies CRC32. On success: `rp2040.restart()`.
7. Console-esp32 waits for UART HELLO within ~5 s to confirm the new firmware is running.

### 2. pad-esp32 — OTA fetch over WiFi

Since the pad-esp32 connects to the console-esp32 WiFi AP, it can HTTP-GET its own firmware
from the console-esp32 without any external server.

**Trigger path:**
1. Console-esp32 web UI receives `POST /ota/upload-pad`; binary stored in LittleFS.
2. Console-esp32 sends `LP_MSG_OTA_BEGIN` (0x50) over TCP with payload `[size LE4][crc32 LE4]`.
3. Pad-esp32 calls `httpUpdate.update("http://192.168.4.1/ota/pad-esp32.bin")`.
4. Pad-esp32 self-flashes and reboots. It reconnects automatically once done.

**Notes:**
- The pad-esp32 already has OTA partition support (ESP32-S2 with 4MB flash); the partition
  table needs to be confirmed / updated to include OTA0 + OTA1 + LittleFS.
- Alternatively, the console-esp32 can stream the binary on-demand (the pad-esp32 opens an
  HTTP connection and the console-esp32 serves the bytes as they are uploaded from the
  browser), avoiding the need to store the full binary in LittleFS.

### 3. console-esp32 — Self OTA (applied last)

Standard Arduino ESP32 OTA using the `Update` library.

**Trigger path:**
1. Web UI receives `POST /ota/upload-console` with the new firmware binary.
2. Console-esp32 pipes it through `Update.write()` into the OTA1 partition.
3. On completion, `Update.end()` marks the OTA1 partition as the next boot target.
4. Console-esp32 calls `ESP.restart()` and comes back on the new firmware.

**Must run last** — once it restarts, it can no longer coordinate the other chips.

---

## UART DFU Protocol (console-esp32 → console-rp2040)

LP frames handle signalling. A separate lightweight chunked format carries the firmware
bytes, because `LP_MAX_PAYLOAD = 64` would make a full-image transfer impractically slow.

### Signalling (LP frames)

| Opcode | Direction | Payload | Meaning |
|---|---|---|---|
| `LP_MSG_OTA_BEGIN`  0x50 | ESP32 → RP2040 (UART) | `[size LE4][crc32 LE4]` | Start DFU; RP2040 calls `Updater.begin(size)`, replies ACK |
| `LP_MSG_OTA_COMMIT` 0x51 | ESP32 → RP2040 (UART) | _(empty)_ | All chunks sent; RP2040 calls `Updater.end()`, verifies CRC32, reboots |
| `LP_MSG_OTA_BEGIN`  0x50 | ESP32 → pad-esp32 (TCP) | `[size LE4][crc32 LE4]` | Trigger pad-esp32 HTTP OTA fetch |

### Data chunks (raw, over same UART — outside LP framing)

After the RP2040 ACKs `LP_MSG_OTA_BEGIN`, the UART switches to raw DFU chunk mode.
Each chunk:

```
Offset  Size  Field
──────  ────  ──────────────────────────────────────────────────────
0       2     Magic: 0xDF 0xC0
2       4     Byte offset in firmware image (LE uint32)
6       2     Chunk data length in bytes, max 1024 (LE uint16)
8       N     Firmware bytes  (N = length field)
8+N     2     CRC16-CCITT of the N data bytes (LE uint16)
```

RP2040 responds to each chunk with one byte:
- `0x06` — ACK (chunk accepted, passed to `Updater.write()`)
- `0x15` — NAK (CRC mismatch; console-esp32 retransmits the same chunk)

The console-esp32 waits for ACK/NAK before sending the next chunk (up to 3 retries per
chunk, then aborts). After all chunks are ACKed it sends `LP_MSG_OTA_COMMIT`.

### Timeouts

- **RP2040**: if no chunk arrives within 10 s after ACKing `LP_MSG_OTA_BEGIN`, the DFU
  session is silently aborted. `Updater` is not committed so current firmware is safe.
- **Console-esp32**: 3 s per-chunk ACK timeout, 3 retries, then abort.

### Flash layout — XIAO RP2040 (2 MB)

The arduino-pico `Updater` stores the incoming image in the upper half of flash and only
marks it for boot on a successful `Updater.end()` — safe against mid-transfer power loss.

```
0x10000000  ┌───────────────────────────────┐
            │  boot2 (256 B)                │
            │  running firmware             │  ≤ 1 MB
0x10100000  ├───────────────────────────────┤
            │  Updater staging area         │  ≤ 1 MB
            │  (new image written here)     │
0x10200000  └───────────────────────────────┘
```

---

## Update Sequence

```
User uploads firmware package to web UI
          │
          ▼
[1] POST /ota/upload-rp2040  (binary stored in LittleFS)
    ESP32 → (UART LP_MSG_OTA_BEGIN[size,crc32]) → RP2040
    RP2040: Updater.begin(size) → ACK
    ESP32 streams DFU chunks (1024 B each, CRC16 per chunk)
    ESP32 → (UART LP_MSG_OTA_COMMIT) → RP2040
    RP2040: Updater.end(), verify CRC32 → rp2040.restart()
          │
          ▼  (~5 s — RP2040 reboots with new firmware, no user action)
[2] RP2040 UART HELLO received → confirmed
          │
          ▼
[3] POST /ota/upload-pad  (binary stored in LittleFS)
    ESP32 → (TCP LP_MSG_OTA_BEGIN[size,crc32]) → pad-esp32
    pad-esp32: httpUpdate("http://192.168.4.1/ota/pad-esp32.bin") → reboots
    pad-esp32 reconnects → confirmed
          │
          ▼
[4] POST /ota/upload-console
    console-esp32 applies OTA via Update library → ESP.restart()
    Done.
```

---

## Required Changes

### shared/include/link_protocol.h
Add two new LP message types:
```c
LP_MSG_OTA_BEGIN  = 0x50,  // ESP32→RP2040 (UART): begin DFU, payload=[size LE4][crc32 LE4]
                           // ESP32→pad-esp32 (TCP): trigger HTTP OTA fetch
LP_MSG_OTA_COMMIT = 0x51,  // ESP32→RP2040 (UART): all chunks sent, commit and reboot
```

### firmware/console-rp2040/src/main.cpp
- Handle `LP_MSG_OTA_BEGIN`: call `Updater.begin(size)`, reply ACK, switch to chunk-receive loop.
- Receive raw DFU chunks: `Updater.write()` per chunk, respond `0x06`/`0x15` per chunk.
- Handle `LP_MSG_OTA_COMMIT`: call `Updater.end()`, verify CRC32, call `rp2040.restart()`.
- 10 s idle timeout to abort safely if ESP32 disconnects mid-transfer.
- Enable OTA staging in `platformio.ini` (sufficient flash reserved for upper-half staging).

### firmware/pad-esp32/src/main.cpp
- Handle `LP_MSG_OTA_BEGIN` (TCP): call `httpUpdate.update("http://192.168.4.1/ota/pad-esp32.bin")`.
- Ensure OTA partitions are present in the partition CSV.

### firmware/console-esp32/platformio.ini + partition table
- Custom partition CSV: OTA0 + OTA1 (≥ 1.5 MB each) + LittleFS (~960 KB).
- Add `LittleFS` for binary storage and `Update` library for self-flash.
- Add web UI routes: `POST /ota/upload-rp2040`, `POST /ota/upload-pad`,
  `POST /ota/upload-console`, `GET /ota/pad-esp32.bin`.

### firmware/console-esp32/src/main.cpp
- `POST /ota/upload-rp2040`: save to LittleFS, send `LP_MSG_OTA_BEGIN` over UART, stream
  DFU chunks (1024 B + CRC16 each), send `LP_MSG_OTA_COMMIT`, wait for UART HELLO.
- `GET /ota/pad-esp32.bin`: serve binary from LittleFS.
- `POST /ota/upload-pad`: save to LittleFS, send `LP_MSG_OTA_BEGIN` over TCP.
- `POST /ota/upload-console`: stream through `Update` library, `ESP.restart()`.

---

## Flash Layout

### console-esp32 (4 MB)

Current app binary: ~838 KB. A custom partition table is needed to fit two app partitions
+ LittleFS large enough to hold the pad-esp32 binary:

| Name      | Type | SubType | Size    | Notes                        |
|-----------|------|---------|---------|------------------------------|
| nvs       | data | nvs     | 20 KB   |                              |
| otadata   | data | ota     | 8 KB    |                              |
| app0      | app  | ota_0   | 1.5 MB  | Running firmware             |
| app1      | app  | ota_1   | 1.5 MB  | OTA staging target           |
| littlefs  | data | spiffs  | ~960 KB | pad-esp32.bin + rp2040 bin   |

Total: ≈ 4 MB. The pad binary (838 KB) fits in 960 KB LittleFS with ~120 KB spare.
Streaming the pad binary on-the-fly (no LittleFS storage) removes this constraint
entirely but requires the pad to be connected during the upload step.

### console-rp2040 (XIAO RP2040, 2 MB)

See the flash layout diagram in the UART DFU Protocol section above. arduino-pico's
`Updater` handles the staging area automatically; no manual partition table required.

---

## Open Questions / Decisions Needed

1. **Bundle vs. separate uploads** — Single firmware package (zip or custom container
   unpacked by console-esp32) or three separate upload buttons? A bundle is more
   user-friendly but adds parsing complexity on the constrained ESP32.

2. **Store or stream pad binary** — Storing in LittleFS allows retry and decouples upload
   timing from the pad being online, but requires the tight partition layout above.
   Streaming avoids storage but means the pad must be connected during upload.

3. **UART baud rate during DFU** — 115200 baud gives ~11 KB/s. The RP2040 binary is small
   so this is fine. If we ever route pad-esp32 updates through UART (unlikely) we'd want
   higher baud; 921600 gives ~90 KB/s if the hardware traces support it.

4. **Rollback** — The ESP32 Arduino `Update` library supports automatic rollback if
   `Update.end(true)` is used with `esp_ota_mark_app_valid_cancel_rollback()` called on
   successful boot. Worth wiring up for the console-esp32 self-update.
