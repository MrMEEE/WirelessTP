# Toy Pad Link Protocol v1

This protocol is used across two logical links:

- Link A: pad ESP32 <-> console ESP32 over Wi-Fi (UDP)
- Link B: console ESP32 <-> console RP2040 over UART

Wi-Fi mode for Link A:

- Console ESP32 is a SoftAP (no external access point required).
- Pad ESP32 joins console SoftAP as station after provisioning.

## Frame format

- `sync0` = 0x4c
- `sync1` = 0x44
- `version` = 0x01
- `type` = message type
- `seq` = per-sender monotonically increasing byte
- `length` = payload length (0..64)
- `payload[length]`
- `crc16` = CCITT over header + payload

## Message types

- `0x01 HELLO`
- `0x10 TAG_SET`
- `0x11 TAG_CLEAR`
- `0x20 LED_CMD`
- `0x30 PAIR_SET`
- `0x7F ACK`

## Initial reliability strategy

- Every non-ACK frame requests ACK by default.
- Receiver sends ACK with `seq` matching received frame.
- Sender retries up to 3 times with 50 ms spacing.
- If retries fail, sender marks link degraded.

## Event model v1

- TAG_SET: place toy in one of three zones.
- TAG_CLEAR: remove toy from zone.
- LED_CMD: command per-zone RGB/effect.

## Pairing model v1

- Console AP is open (no AP password).
- Pad sends `HELLO` payload with role `0xA1`.
- If pad has no stored secret, payload is `[0xA1]` and console treats this as enrollment.
- Console generates secret and sends `PAIR_SET` payload `[secret_u32_le]`.
- Pad stores secret and sends future `HELLO` as `[0xA1, secret_u32_le]`.
- Console locks session to paired `(ip,port)` and drops other peers.

## Security v1

No crypto for first bring-up. Add HMAC or AES-GCM in v2 once data path is stable.

## Notes

Do not optimize packet size until end-to-end behavior is verified.
Deterministic behavior matters more than throughput for this project.
