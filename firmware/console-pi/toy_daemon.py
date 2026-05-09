#!/usr/bin/env python3
"""
toy_daemon.py — LEGO Dimensions Toy Pad HID daemon for Raspberry Pi gadget

Reads 32-byte OUT reports from /dev/hidg0 (host → pad commands),
responds with 32-byte IN reports (pad → host events).

Run after setup_gadget.sh has created /dev/hidg0:
    sudo python3 toy_daemon.py

Optional: bridge to pad ESP32 via LP UART protocol.
Set --uart /dev/ttyAMA0 (or /dev/ttyUSB0) and --uart-baud 115200.
Without --uart, the daemon runs in standalone mode: it responds correctly
to all Toy Pad commands but never fires tag events (useful for RPCS3 testing).
"""

import argparse
import os
import select
import serial
import struct
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

HIDG_DEV     = "/dev/hidg0"
PACKET_SIZE  = 32

MAGIC_HOST   = 0x55   # command/response traffic
MAGIC_PORTAL = 0x56   # unsolicited pad event traffic

# Startup payload returned by a real Toy Pad in response to B0.
B0_REPLY_PAYLOAD = bytes([
    0x00, 0x2f, 0x02, 0x01, 0x02, 0x02, 0x04, 0x02,
    0xf5, 0x00, 0x19, 0x8d, 0x54, 0x8e, 0x2d, 0x5b,
    0xae, 0x4e, 0x00, 0x42, 0x17, 0x01, 0x00, 0x15,
])

# LP framing (matches link_protocol.h)
LP_SYNC_0        = 0x4c  # 'L'
LP_SYNC_1        = 0x44  # 'D'
LP_VERSION       = 0x01
LP_MSG_HELLO     = 0x01
LP_MSG_TAG_SET   = 0x10  # console→pi: tag placed
LP_MSG_TAG_CLEAR = 0x11  # console→pi: tag removed
LP_MSG_LED_CMD   = 0x20
LP_MSG_DEBUG     = 0x40
LP_MSG_ACK       = 0x7f

TAG_ACTION_PLACED  = 0x00
TAG_ACTION_REMOVED = 0x01

# Header layout: [sync0][sync1][version][type][seq][length] = 6 bytes
LP_HEADER_SIZE = 6
LP_CRC_SIZE    = 2

# ---------------------------------------------------------------------------
# Toy Pad protocol helpers
# ---------------------------------------------------------------------------

def _checksum(data: bytes) -> int:
    return sum(data) & 0xff


def lp_crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE — matches lp_crc16_ccitt() in link_protocol.h."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def lp_uid_from_toy_id(toy_id: int, zone: int) -> bytes:
    """Synthesise a 7-byte UID from a 32-bit toyId (matches RP2040 buildToyUidFromToyId)."""
    uid = bytearray(7)
    uid[0] = toy_id & 0xff
    uid[1] = (toy_id >> 8) & 0xff
    uid[2] = (toy_id >> 16) & 0xff
    uid[3] = (toy_id >> 24) & 0xff
    uid[4] = 0xa5
    uid[5] = zone & 0xff
    uid[6] = (toy_id ^ 0x5a) & 0xff
    return bytes(uid)


def build_reply(counter: int, payload: bytes = b"") -> bytes:
    header = bytes([MAGIC_HOST, len(payload) + 1, counter])
    body   = header + payload
    cs     = _checksum(body)
    pkt    = body + bytes([cs])
    return pkt.ljust(PACKET_SIZE, b"\x00")


def parse_out_packet(raw: bytes):
    """
    Parse a 32-byte host→pad packet.
    Returns (command, counter, args) on success, None on failure.
    """
    if len(raw) < PACKET_SIZE or raw[0] != MAGIC_HOST:
        return None
    length = raw[1]
    if length < 2:
        return None
    cs_idx = length + 2
    if cs_idx >= PACKET_SIZE:
        return None
    expected = _checksum(raw[:cs_idx])
    if expected != raw[cs_idx]:
        return None
    command = raw[2]
    counter = raw[3]
    args    = raw[4:4 + (length - 2)]
    return command, counter, args


# ---------------------------------------------------------------------------
# HID gadget I/O
# ---------------------------------------------------------------------------

class HidGadget:
    def __init__(self, path: str):
        # Non-blocking FD avoids deadlocks when host writes quickly but does not
        # consume IN replies at the same rate.
        self._fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        print(f"[hid] opened {path}")

    def read_report(self, timeout: float = 1.0):
        try:
            r, _, _ = select.select([self._fd], [], [], timeout)
        except OSError:
            return None
        if not r:
            return None
        try:
            data = os.read(self._fd, PACKET_SIZE)
        except (BlockingIOError, OSError):
            return None
        if len(data) < PACKET_SIZE:
            data = data.ljust(PACKET_SIZE, b"\x00")
        return bytes(data)

    def write_report(self, data: bytes):
        if len(data) != PACKET_SIZE:
            data = data.ljust(PACKET_SIZE, b"\x00")[:PACKET_SIZE]
        try:
            os.write(self._fd, data)
            return True
        except (BlockingIOError, OSError):
            return False


# ---------------------------------------------------------------------------
# LP UART bridge (optional)
# ---------------------------------------------------------------------------

class LpBridge:
    """
    Reads LP-framed messages from the pad ESP32 over UART and enqueues
    tag events for the Toy Pad state machine.
    """

    def __init__(self, port: str, baud: int, debug_forward: bool = False):
        self._ser = serial.Serial(port, baud, timeout=0.1)
        self._lock = threading.Lock()
        self._events = []   # list of (action, zone, uid_bytes)
        self._seq = 0
        self._debug_forward = debug_forward
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        print(f"[lp] UART bridge on {port} @ {baud}")

    def pop_events(self):
        with self._lock:
            evts, self._events = self._events, []
        return evts

    def _next_seq(self) -> int:
        with self._lock:
            s = self._seq
            self._seq = (self._seq + 1) & 0xff
        return s

    def send_debug(self, msg: str):
        if not self._debug_forward:
            return
        payload = msg.encode("utf-8", errors="replace")[:64]
        self._send_frame(LP_MSG_DEBUG, payload)

    def _send_frame(self, msg_type: int, payload: bytes):
        payload = payload[:64]
        seq = self._next_seq()
        header = bytes([
            LP_SYNC_0,
            LP_SYNC_1,
            LP_VERSION,
            msg_type & 0xff,
            seq,
            len(payload) & 0xff,
        ])
        body = header + payload
        crc = lp_crc16_ccitt(body)
        frame = body + struct.pack("<H", crc)
        self._ser.write(frame)

    def send_led_cmd(self, zone: int, r: int, g: int, b: int, effect_id: int = 0):
        # LP LED_CMD payload: [zone_id, r, g, b, effect_id]
        payload = bytes([
            zone & 0xff,
            r & 0xff,
            g & 0xff,
            b & 0xff,
            effect_id & 0xff,
        ])
        self._send_frame(LP_MSG_LED_CMD, payload)

    def _reader(self):
        buf = bytearray()
        while True:
            try:
                chunk = self._ser.read(64)
            except serial.SerialException as e:
                print(f"[lp] UART read error: {e}", file=sys.stderr)
                time.sleep(1)  # Retry after delay
                continue
            if not chunk:
                continue
            buf.extend(chunk)
            buf = self._parse(buf)

    def _parse(self, buf: bytearray) -> bytearray:
        # Frame layout: [sync0][sync1][version][type][seq][length][payload...][crc_lo][crc_hi]
        # Header = 6 bytes, CRC = 2 bytes, total = 6 + length + 2
        while True:
            # Need at least a full header to do anything
            if len(buf) < LP_HEADER_SIZE:
                break

            # Scan for sync pair
            idx = -1
            for i in range(len(buf) - 1):
                if buf[i] == LP_SYNC_0 and buf[i + 1] == LP_SYNC_1:
                    idx = i
                    break
            if idx == -1:
                # No sync found — discard all but the last byte
                buf = buf[-1:]
                break
            if idx > 0:
                buf = buf[idx:]

            if len(buf) < LP_HEADER_SIZE:
                break

            version     = buf[2]
            msg_type    = buf[3]
            # buf[4] = seq (ignored by receiver)
            payload_len = buf[5]   # single uint8, NOT a uint16

            if version != LP_VERSION:
                # Bad version — skip past this sync pair and keep scanning
                buf = buf[1:]
                continue

            total = LP_HEADER_SIZE + payload_len + LP_CRC_SIZE
            if len(buf) < total:
                # Incomplete frame — wait for more bytes
                break

            frame_data  = bytes(buf[:total])
            crc_received = struct.unpack_from("<H", frame_data, LP_HEADER_SIZE + payload_len)[0]
            crc_calc     = lp_crc16_ccitt(frame_data[:LP_HEADER_SIZE + payload_len])
            buf = buf[total:]

            if crc_received != crc_calc:
                print(f"[lp] bad CRC (got {crc_received:#06x} expected {crc_calc:#06x}), dropping frame")
                continue

            payload = frame_data[LP_HEADER_SIZE:LP_HEADER_SIZE + payload_len]

            # TAG_SET payload: [zone (1 byte), toyId (4 bytes LE)]
            if msg_type == LP_MSG_TAG_SET and len(payload) >= 5:
                zone   = payload[0]
                toy_id = struct.unpack_from("<I", payload, 1)[0]
                uid    = lp_uid_from_toy_id(toy_id, zone)
                with self._lock:
                    self._events.append((TAG_ACTION_PLACED, zone, uid))

            # TAG_CLEAR payload: [zone (1 byte), toyId (4 bytes LE)]
            elif msg_type == LP_MSG_TAG_CLEAR and len(payload) >= 5:
                zone   = payload[0]
                toy_id = struct.unpack_from("<I", payload, 1)[0]
                uid    = lp_uid_from_toy_id(toy_id, zone)
                with self._lock:
                    self._events.append((TAG_ACTION_REMOVED, zone, uid))

            # Silently ignore HELLO, ACK, DEBUG, and any other type

        return buf


# ---------------------------------------------------------------------------
# Toy Pad state machine
# ---------------------------------------------------------------------------

class ToyPadDaemon:
    def __init__(self, hid: HidGadget, bridge: LpBridge | None, verbose: bool = False, keepalive_ms: int = 100):
        self._hid    = hid
        self._bridge = bridge
        self._verbose = verbose
        self._keepalive_interval = (keepalive_ms / 1000.0) if keepalive_ms > 0 else 0.0
        self._last_keepalive = time.monotonic()
        self._saw_host_command = False
        # Re-send the most recent valid IN report as an idle keepalive.
        # This avoids inventing protocol bytes while keeping EP1 active.
        self._last_in_report = build_reply(0)

    def _log(self, msg: str):
        if not self._verbose:
            return
        print(msg)
        if self._bridge:
            try:
                # Keep lines short so they fit LP_MAX_PAYLOAD in one frame.
                self._bridge.send_debug(msg[:64])
            except Exception:
                # Never break Toy Pad emulation if debug forwarding fails.
                pass

    def run(self):
        self._log("[toy] running - waiting for host commands")
        while True:
            # Drain tag events from the bridge and send unsolicited IN packets
            if self._bridge:
                for action, zone, uid in self._bridge.pop_events():
                    self._send_tag_event(action, zone, uid)

            raw = self._hid.read_report(timeout=0.05)
            if raw is None:
                if self._keepalive_interval > 0:
                    now = time.monotonic()
                    if (now - self._last_keepalive) >= self._keepalive_interval:
                        if self._hid.write_report(self._last_in_report):
                            self._last_keepalive = now
                            if self._verbose:
                                self._log("[toy] keepalive IN report sent")
                continue

            result = parse_out_packet(raw)
            if result is None:
                self._log(f"[toy] bad packet: {raw[:8].hex()}")
                continue

            command, counter, args = result
            self._saw_host_command = True
            reply = self._handle(command, counter, args)
            if reply:
                wrote = self._hid.write_report(reply)
                if self._verbose and wrote:
                    self._log(f"[toy] CMD {command:#04x} ctr={counter} -> replied")
                    self._last_in_report = reply
                    self._last_keepalive = time.monotonic()
                elif self._verbose and not wrote:
                    self._log(f"[toy] CMD {command:#04x} ctr={counter} -> reply dropped (IN busy)")
                elif wrote:
                    self._last_in_report = reply
                    self._last_keepalive = time.monotonic()

    def _handle(self, command: int, counter: int, args: bytes):
        if command == 0xb0:
            return build_reply(counter, B0_REPLY_PAYLOAD)

        if command == 0xb1:
            # Seed/challenge — reply with empty payload (auth not implemented)
            return build_reply(counter)

        if command == 0xb3:
            return build_reply(counter)

        if command == 0xc0:
            # Set color: args = [pad, r, g, b]
            if self._bridge and len(args) >= 4:
                pad = args[0]
                r = args[1]
                g = args[2]
                b = args[3]

                # USB pad index uses 1..3; LP LED zone uses 0..2.
                # Use 0xff for "all zones".
                if pad == 0:
                    zone = 0xff
                elif 1 <= pad <= 3:
                    zone = pad - 1
                else:
                    zone = 0xff

                try:
                    self._bridge.send_led_cmd(zone, r, g, b, effect_id=0)
                except Exception:
                    # Never block/abort HID handling if UART forwarding fails.
                    pass

            if self._verbose and len(args) >= 4:
                self._log(f"[toy]   color pad={args[0]} rgb=({args[1]},{args[2]},{args[3]})")
            return build_reply(counter)

        if command == 0xc2:
            # Fade color
            return build_reply(counter)

        if command == 0xc3:
            # Flash
            return build_reply(counter)

        if command == 0xc8:
            # Get color
            return build_reply(counter)

        if command == 0xd2:
            # Read tag page
            return build_reply(counter)

        if command == 0xd3:
            # Write tag page
            return build_reply(counter)

        # Unknown command — ACK it anyway so the host doesn't hang
        self._log(f"[toy]   unknown command {command:#04x}, sending empty ACK")
        return build_reply(counter)

    def _send_tag_event(self, action: int, zone: int, uid: bytes):
        # Unsolicited IN packet — tag placed or removed.
        # Real Toy Pad event format:
        #   [0x56, 0x0b, 0x56, pad_idx, action, uid[0..6], checksum, 0...]
        # Length byte (0x0b=11) counts from cmd byte through checksum inclusive:
        #   cmd(1) + pad_idx(1) + action(1) + uid(7) + checksum(1) = 11
        # Note: NO counter byte — unsolicited events have only 3 header bytes.
        toy_zone = zone + 1  # LP zone 0/1/2 → Toy Pad zone 1/2/3
        content = bytes([toy_zone, action]) + uid[:7]   # 9 bytes
        pkt = bytes([MAGIC_PORTAL, len(content) + 2, 0x56]) + content
        cs  = _checksum(pkt)
        pkt = (pkt + bytes([cs])).ljust(PACKET_SIZE, b"\x00")
        wrote = self._hid.write_report(pkt)
        if wrote:
            self._last_in_report = pkt
            self._last_keepalive = time.monotonic()
        verb = "placed" if action == TAG_ACTION_PLACED else "removed"
        if self._verbose and wrote:
            self._log(f"[toy] TAG {verb} zone={toy_zone} uid={uid.hex()}")
        elif self._verbose and not wrote:
            self._log(f"[toy] TAG {verb} zone={toy_zone} dropped (IN busy)")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LEGO Toy Pad HID daemon")
    parser.add_argument("--hid",       default=HIDG_DEV, help="HID gadget device (default: /dev/hidg0)")
    parser.add_argument("--uart",      default=None,      help="UART port for LP bridge, e.g. /dev/ttyAMA0")
    parser.add_argument("--uart-baud", default=115200, type=int, help="UART baud rate (default: 115200)")
    parser.add_argument("--keepalive-ms", default=100, type=int,
                        help="Periodic IN keepalive interval in ms after first host command (0 disables; default: 100)")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose runtime logging")
    parser.add_argument("--bridge-debug", action="store_true", help="Forward verbose logs over LP debug channel")
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("WARNING: not running as root; /dev/hidg0 may be unreadable", file=sys.stderr)

    hid = HidGadget(args.hid)

    bridge = None
    if args.uart:
        bridge = LpBridge(args.uart, args.uart_baud, debug_forward=args.bridge_debug)
        bridge.send_debug("[toy] uart bridge online")
    else:
        print("[toy] standalone mode — no UART bridge (tag events disabled)")

    ToyPadDaemon(hid, bridge, verbose=args.verbose, keepalive_ms=args.keepalive_ms).run()


if __name__ == "__main__":
    main()
