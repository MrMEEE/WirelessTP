# Bring-up Checklist (Direct AP + Pairing)

## 1) Flash order

1. Flash console RP2040 firmware.
2. Flash console ESP32 firmware.
3. Flash pad ESP32 firmware.

## 2) Configure pairing defaults

No pair code or AP password setup is required.

Console ESP32 creates an open AP named `ToyPadConsole-xxxxxx`.

## 3) Console-side UART wiring

- ESP32 TX -> RP2040 RX
- ESP32 RX <- RP2040 TX
- GND shared

## 4) Provision pad over local setup portal

1. Power pad ESP32 with no saved config.
2. Join Wi-Fi network `ToyPad-Setup` (password `toypadsetup`).
3. Open `http://192.168.4.1`.
4. Select console SSID (`ToyPadConsole-xxxxxx`), leave password empty for open AP.
5. Save and reboot. On first link, secret enrollment happens automatically.

## 5) Reset procedure (pad)

- Hold BOOT (GPIO0 low) for about 3 seconds during boot.
- Saved config is cleared and setup portal starts again.

## 6) Expected serial logs

### RP2040

- boot line
- periodic `link hello`
- periodic `TAG_SET zone=... toyId=...`

### pad ESP32

- station connected line
- `sent virtual TAG_SET ...`
- `ack received`

### console ESP32

- AP SSID (open) printed
- `paired with <ip>:<port>`
- no repeated bad frame logs

## 7) Failure symptoms

- `ack timeout` on pad: console ESP32 not reachable or frame rejected.
- `unpaired client ignored` on console: malformed HELLO or failed secret verification.
- `dropped bad UDP frame`: datagram corruption or protocol mismatch.
- `dropped bad crc` on RP2040: UART wiring/noise or baud mismatch.

## 8) Next implementation step

Replace RP2040 TODO USB hook with TinyUSB Toy Pad descriptors and endpoint handling.
