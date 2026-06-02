#!/usr/bin/env python3
"""
Xbox 360 LEGO Dimensions Toy Pad - Protocol Probe
===================================================
Connects to the real Xbox 360 Toy Pad (VID=0x24C6, PID=0xFA01) and
probes the data protocol on interface 0 to determine:

  1. Whether raw 0x55-based payloads work (same as PS3), or
  2. Whether an Xbox-style [0x00, 0x14] / [0x0B, 0x14] prefix is needed.
  3. Whether XSM3 auth on interface 3 is required before the pad responds.

Run with sudo, or add a udev rule:
  echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="24c6", ATTR{idProduct}=="fa01", MODE="0666"' \
    | sudo tee /etc/udev/rules.d/99-lego-xbox.rules && sudo udevadm control --reload

Usage:
  sudo python3 probe-xbox-toypad.py [--xsm3-stub]

Options:
  --xsm3-stub   Send dummy XSM3 control responses before data (experimental)
"""

import sys
import time
import argparse
import usb.core
import usb.util

# ── Device identity ────────────────────────────────────────────────────────────
VID = 0x24C6
PID = 0xFA01

# ── Interface / endpoint map ───────────────────────────────────────────────────
IFACE_DATA   = 0   # FF/5D/01 — interrupt IN+OUT
IFACE_AUDIO  = 1   # FF/5D/03 — 4 EPs (not used here)
IFACE_CHAT   = 2   # FF/5D/02 — interrupt IN only (not used here)
IFACE_XSM3   = 3   # FF/FD/13 — control channel, no EPs

EP_DATA_IN   = 0x81  # interrupt IN  32B interval=4
EP_DATA_OUT  = 0x01  # interrupt OUT 32B interval=4

TIMEOUT_MS   = 1000

# ── XSM3 control request codes (bRequest values) ──────────────────────────────
XSM3_GET_ID       = 0x81  # IN  — static device ID
XSM3_CHALLENGE1   = 0x82  # OUT — send challenge 1
XSM3_GET_RESP1    = 0x83  # IN  — get response 1
XSM3_AUTH_SUCCESS = 0x84  # IN  — auth success notification
XSM3_GET_STATUS   = 0x86  # IN  — 0x01=busy, 0x02=ready
XSM3_CHALLENGE2   = 0x87  # OUT — send challenge 2 verify


# ── PS3-compatible 0x55 protocol helpers ──────────────────────────────────────

def checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def build_command(cmd: int, counter: int, args: bytes = b'') -> bytes:
    """Build a raw 32-byte pad command (PS3 / 0x55 wire format)."""
    length = 2 + len(args)   # cmd byte + counter byte + args
    header = bytes([0x55, length, cmd, counter]) + bytes(args)
    cs = checksum(header)
    return (header + bytes([cs])).ljust(32, b'\x00')


def parse_response(data: bytes):
    """
    Parse a raw 32-byte response.
    Returns (dict, None) on success or (None, error_string) on failure.
    """
    if len(data) < 4:
        return None, f"too short ({len(data)} bytes)"
    magic = data[0]
    if magic not in (0x55, 0x56):
        return None, f"unexpected magic 0x{magic:02x} (expected 0x55 or 0x56)"
    length = data[1]
    counter_echo = data[2]
    if 2 + length >= len(data):
        return None, f"length {length} overflows packet"
    payload = bytes(data[3 : 2 + length])
    cs_pos = 2 + length
    cs_actual   = data[cs_pos]
    cs_expected = checksum(data[:cs_pos])
    return {
        'magic':   magic,
        'counter': counter_echo,
        'payload': payload,
        'cs_ok':   cs_actual == cs_expected,
        'cs_exp':  cs_expected,
        'cs_got':  cs_actual,
    }, None


def parse_xbox_framed(data: bytes):
    """
    Try to parse as Xbox-framed:
      OUT framing: [0x00, 0x14, <30-byte payload>]
      IN  framing: [0x0B, 0x14, <30-byte payload>]
    Returns the inner 30 bytes if first two bytes match either framing,
    else None.
    """
    if len(data) >= 32 and data[0] in (0x00, 0x0B) and data[1] == 0x14:
        return bytes(data[2:32])
    return None


def hex_dump(label: str, data: bytes, n: int = 20):
    print(f"  {label}: {data[:n].hex(' ')}{'…' if len(data) > n else ''}")


# ── Main probe ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-xsm3', action='store_true',
                    help='Skip XSM3 handshake (default: always attempt XSM3 stub)')
    args = ap.parse_args()

    print(f"Searching for Xbox LEGO Dimensions Toy Pad {VID:04x}:{PID:04x}...")
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("ERROR: Device not found. Is it connected? Try sudo?")

    print(f"Found: '{dev.manufacturer}' — '{dev.product}'  "
          f"(Bus {dev.bus}, Dev {dev.address})")

    # Detach kernel driver from ALL interfaces (xpad may grab multiple)
    ALL_IFACES = [IFACE_DATA, IFACE_AUDIO, IFACE_CHAT, IFACE_XSM3]
    reattach = []
    for iface in ALL_IFACES:
        try:
            if dev.is_kernel_driver_active(iface):
                print(f"Detaching kernel driver from interface {iface}...")
                dev.detach_kernel_driver(iface)
                reattach.append(iface)
        except usb.core.USBError:
            pass

    # Do NOT call set_configuration() — it triggers xpad rebind even after detach.
    # The device is already configured at this point.

    # Claim interface 0 for data
    usb.util.claim_interface(dev, IFACE_DATA)
    print(f"Claimed interface {IFACE_DATA} (data channel)")

    # XSM3 handshake stub on interface 3 (control channel)
    if not args.no_xsm3:
        print("\n── XSM3 handshake stub (interface 3, control requests) ─────────")
        _run_xsm3_stub(dev)

    print()
    try:
        _run_data_probe(dev)
    finally:
        usb.util.release_interface(dev, IFACE_DATA)
        for iface in reattach:
            try:
                dev.attach_kernel_driver(iface)
            except Exception:
                pass
        print("\nReleased interface. Done.")


def _run_xsm3_stub(dev):
    """
    Walk through the XSM3 control-request handshake on interface 3.
    We are playing the CONSOLE role: challenge the device, accept whatever
    response it gives (no actual crypto verification), declare success.
    """
    # Claim interface 3 (needed for control transfers targeting it)
    try:
        if dev.is_kernel_driver_active(IFACE_XSM3):
            dev.detach_kernel_driver(IFACE_XSM3)
        usb.util.claim_interface(dev, IFACE_XSM3)
        print("  Claimed interface 3 (XSM3)")
    except usb.core.USBError as e:
        print(f"  WARNING: could not claim interface 3: {e}")

    # Try both common wValue variants
    for wval_label, wval in [("wValue=0x0100", 0x0100), ("wValue=0x0000", 0x0000)]:
        print(f"\n  Trying {wval_label}:")

        def ctrl_in(bRequest, wLength=20, label='', wValue=wval):
            for bmRT in [0xC1, 0xC0]:  # Interface recipient, then Device recipient
                try:
                    data = dev.ctrl_transfer(
                        bmRequestType=bmRT,
                        bRequest=bRequest,
                        wValue=wValue,
                        wIndex=IFACE_XSM3,
                        data_or_wLength=wLength,
                        timeout=TIMEOUT_MS,
                    )
                    r = bytes(data)
                    print(f"    XSM3 GET 0x{bRequest:02x} ({label}) bmRT=0x{bmRT:02x}: {r.hex()}")
                    return r
                except usb.core.USBError as e:
                    print(f"    XSM3 GET 0x{bRequest:02x} ({label}) bmRT=0x{bmRT:02x}: {e}")
            return None

        def ctrl_out(bRequest, data=b'', label='', wValue=wval):
            for bmRT in [0x41, 0x40]:
                try:
                    n = dev.ctrl_transfer(
                        bmRequestType=bmRT,
                        bRequest=bRequest,
                        wValue=wValue,
                        wIndex=IFACE_XSM3,
                        data_or_wLength=bytes(data),
                        timeout=TIMEOUT_MS,
                    )
                    print(f"    XSM3 SET 0x{bRequest:02x} ({label}) bmRT=0x{bmRT:02x}: wrote {n} bytes")
                    return n
                except usb.core.USBError as e:
                    print(f"    XSM3 SET 0x{bRequest:02x} ({label}) bmRT=0x{bmRT:02x}: {e}")
            return None

        ctrl_in(XSM3_GET_ID, 20, 'GET_ID')
        ctrl_out(XSM3_CHALLENGE1, b'\x00' * 20, 'CHALLENGE1')
        time.sleep(0.1)
        ctrl_in(XSM3_GET_STATUS, 4, 'GET_STATUS')
        ctrl_in(XSM3_GET_RESP1, 20, 'GET_RESP1')
        ctrl_out(XSM3_CHALLENGE2, b'\x00' * 20, 'CHALLENGE2')
        ctrl_in(XSM3_AUTH_SUCCESS, 4, 'AUTH_SUCCESS')

    try:
        usb.util.release_interface(dev, IFACE_XSM3)
    except Exception:
        pass
    print("\n  XSM3 stub complete (dummy, no real crypto)")
    time.sleep(0.1)


def _run_data_probe(dev):
    counter = [1]

    def send_recv(label: str, pkt: bytes, listen_ms: int = 200) -> list:
        """Send pkt on EP_DATA_OUT, collect all responses for listen_ms ms."""
        hex_dump(f"{label} →", pkt, 20)
        try:
            dev.write(EP_DATA_OUT, pkt, TIMEOUT_MS)
        except usb.core.USBError as e:
            print(f"  write error: {e}")
            return []

        responses = []
        deadline = time.monotonic() + listen_ms / 1000.0
        while time.monotonic() < deadline:
            try:
                r = bytes(dev.read(EP_DATA_IN, 32, 200))
                responses.append(r)
                hex_dump(f"  {label} ←", r, 20)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError as e:
                print(f"  read error: {e}")
                break

        if not responses:
            print(f"  {label} ← (no response)")
        counter[0] += 1
        return responses

    def interpret(label: str, responses: list):
        for r in responses:
            # Try raw 0x55 format
            parsed, err = parse_response(r)
            if parsed:
                flag = 'OK' if parsed['cs_ok'] else f"BAD_CS exp={parsed['cs_exp']:02x} got={parsed['cs_got']:02x}"
                print(f"  [{label}] raw-0x55  ctr={parsed['counter']} "
                      f"payload={parsed['payload'].hex()} cs={flag}")
                continue
            # Try Xbox-framed format
            inner = parse_xbox_framed(r)
            if inner:
                parsed2, err2 = parse_response(inner)
                if parsed2:
                    flag = 'OK' if parsed2['cs_ok'] else f"BAD_CS exp={parsed2['cs_exp']:02x} got={parsed2['cs_got']:02x}"
                    print(f"  [{label}] xbox-framed inner ctr={parsed2['counter']} "
                          f"payload={parsed2['payload'].hex()} cs={flag}")
                else:
                    print(f"  [{label}] xbox-framed but inner parse failed: {err2}")
                continue
            # Unknown
            print(f"  [{label}] unrecognized format: {r[:20].hex(' ')}")

    print("\n══════════════════════════════════════════════════════════════")
    print(" PASS 0 — passive listen (2 s) — any spontaneous IN data?")
    print("══════════════════════════════════════════════════════════════")
    start = time.monotonic()
    while time.monotonic() - start < 2.0:
        try:
            r = bytes(dev.read(EP_DATA_IN, 32, 200))
            hex_dump("spontaneous ←", r, 32)
        except usb.core.USBTimeoutError:
            pass
        except usb.core.USBError as e:
            print(f"  listen error: {e}")
            break

    print("\n══════════════════════════════════════════════════════════════")
    print(" PASS 1 — raw 0x55 format (same as PS3)")
    print("══════════════════════════════════════════════════════════════")

    # ── B0: Wake/Init ────────────────────────────────────────────────────────
    print("\n─── B0: Wake / Init ───")
    pkt = build_command(0xB0, counter[0], b'(c) LEGO 2014')
    r = send_recv('B0-raw', pkt, listen_ms=500)
    interpret('B0-raw', r)

    # ── B1: Challenge round 1 ────────────────────────────────────────────────
    print("\n─── B1: Challenge 1 (dummy bytes) ───")
    pkt = build_command(0xB1, counter[0], bytes(range(8)))
    r = send_recv('B1-raw', pkt, listen_ms=300)
    interpret('B1-raw', r)

    # ── B3: Challenge round 2 ────────────────────────────────────────────────
    print("\n─── B3: Challenge 2 (dummy bytes) ───")
    pkt = build_command(0xB3, counter[0], bytes(range(8)))
    r = send_recv('B3-raw', pkt, listen_ms=300)
    interpret('B3-raw', r)

    # ── C0: Set all zones blue ────────────────────────────────────────────────
    print("\n─── C0: Set LED (all zones, blue) ───")
    pkt = build_command(0xC0, counter[0], bytes([0x00, 0x00, 0x00, 0xFF]))
    r = send_recv('C0-blue', pkt, listen_ms=300)
    interpret('C0-blue', r)
    time.sleep(0.8)

    # ── C0: Set all zones red ────────────────────────────────────────────────
    print("\n─── C0: Set LED (all zones, red) ───")
    pkt = build_command(0xC0, counter[0], bytes([0x00, 0xFF, 0x00, 0x00]))
    r = send_recv('C0-red', pkt, listen_ms=300)
    interpret('C0-red', r)
    time.sleep(0.8)

    # ── C0: off ───────────────────────────────────────────────────────────────
    print("\n─── C0: LED off ───")
    pkt = build_command(0xC0, counter[0], bytes([0x00, 0x00, 0x00, 0x00]))
    r = send_recv('C0-off', pkt, listen_ms=300)
    interpret('C0-off', r)

    print("\n══════════════════════════════════════════════════════════════")
    print(" PASS 2 — Xbox-framed format [0x00, 0x14, <30-byte payload>]")
    print("══════════════════════════════════════════════════════════════")

    def xbox_wrap(inner: bytes) -> bytes:
        """Wrap a 30-byte (or less) payload in Xbox framing."""
        return (bytes([0x00, 0x14]) + bytes(inner)[:30]).ljust(32, b'\x00')

    # ── B0 Xbox-framed ───────────────────────────────────────────────────────
    print("\n─── B0: Wake / Init (Xbox-framed) ───")
    inner = build_command(0xB0, counter[0], b'(c) LEGO 2014')[:30]
    pkt = xbox_wrap(inner)
    r = send_recv('B0-xbox', pkt, listen_ms=500)
    interpret('B0-xbox', r)

    # ── C0 Xbox-framed blue ──────────────────────────────────────────────────
    print("\n─── C0: Set LED blue (Xbox-framed) ───")
    inner = build_command(0xC0, counter[0], bytes([0x00, 0x00, 0x00, 0xFF]))[:30]
    pkt = xbox_wrap(inner)
    r = send_recv('C0-xbox-blue', pkt, listen_ms=300)
    interpret('C0-xbox-blue', r)
    time.sleep(0.8)

    # ── C0 off Xbox-framed ───────────────────────────────────────────────────
    print("\n─── C0: LED off (Xbox-framed) ───")
    inner = build_command(0xC0, counter[0], bytes([0x00, 0x00, 0x00, 0x00]))[:30]
    pkt = xbox_wrap(inner)
    r = send_recv('C0-xbox-off', pkt, listen_ms=300)
    interpret('C0-xbox-off', r)

    print("\n══════════════════════════════════════════════════════════════")
    print(" PASS 3 — Unsolicited event listen (5 s)")
    print(" Place/remove a toy to see if tag events appear.")
    print("══════════════════════════════════════════════════════════════")
    print(" (place a LEGO Dimensions figure on the pad now...)")
    start = time.monotonic()
    while time.monotonic() - start < 5.0:
        try:
            r = bytes(dev.read(EP_DATA_IN, 32, 300))
            hex_dump("event ←", r, 32)
            # Try to classify
            parsed, _ = parse_response(r)
            if parsed:
                if parsed['magic'] == 0x56:
                    print(f"           ^ tag event (0x56 magic) payload={parsed['payload'].hex()}")
                else:
                    print(f"           ^ command response payload={parsed['payload'].hex()}")
            else:
                inner = parse_xbox_framed(r)
                if inner:
                    print(f"           ^ xbox-framed inner: {inner[:20].hex(' ')}")
        except usb.core.USBTimeoutError:
            pass
        except usb.core.USBError as e:
            print(f"  listen error: {e}")
            break

    print("\n──────────────────────────────────────────────────────────────")
    print(" Summary hints:")
    print("  - If PASS 1 B0 got a valid response → raw 0x55 works, no Xbox framing needed")
    print("  - If PASS 2 B0 got a valid response → Xbox [0x00,0x14] prefix required")
    print("  - If both timed out → XSM3 auth required first (re-run with --xsm3-stub)")
    print("  - If LEDs changed colour → data path is working!")
    print("──────────────────────────────────────────────────────────────")


if __name__ == '__main__':
    main()
