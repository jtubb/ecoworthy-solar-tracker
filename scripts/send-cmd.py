#!/usr/bin/env python3
"""Send a framed STC command over the ESPHome stream_server TCP bridge.

Wraps an ASCII payload in the Phase 3 wire frame:
    \\xAA\\x55  <payload>  <CRC8 as 2 uppercase hex chars>  \\n

CRC-8/SMBUS (poly 0x07, init 0x00, MSB-first, no reflection, no final XOR),
computed over the payload bytes only.  Same algorithm as the STC firmware's
crc8_update() and the tracker_bridge ESP component.

Usage:
    python scripts/send-cmd.py "!release"
    python scripts/send-cmd.py "!goto az=50 el=50"
    python scripts/send-cmd.py "!cfg get id=1"
    python scripts/send-cmd.py "?"                  # status poll
    python scripts/send-cmd.py --host 192.168.1.194 "!stop"
"""

import argparse
import socket
import sys
import time


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def build_frame(payload: str) -> bytes:
    pb = payload.encode("ascii")
    c = crc8(pb)
    return bytes([0xAA, 0x55]) + pb + f"{c:02X}".encode("ascii") + b"\n"


def hexdump(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def main() -> int:
    p = argparse.ArgumentParser(description="Send a framed STC command.")
    p.add_argument("payload", help='Payload, e.g. "!release"')
    p.add_argument("--host", default="192.168.1.193",
                   help="ESP stream_server host (default 192.168.1.193)")
    p.add_argument("--port", type=int, default=23,
                   help="ESP stream_server port (default 23)")
    p.add_argument("--listen", type=float, default=1.0,
                   help="Seconds to listen for reply after send (default 1.0)")
    args = p.parse_args()

    frame = build_frame(args.payload)
    print(f"payload: {args.payload!r}  ({len(args.payload)} bytes)")
    print(f"crc8:    {frame[-3:-1].decode('ascii')}")
    print(f"sent:    {hexdump(frame)}")

    try:
        s = socket.create_connection((args.host, args.port), timeout=3)
    except OSError as e:
        print(f"error: could not connect to {args.host}:{args.port}: {e}",
              file=sys.stderr)
        return 1

    try:
        s.sendall(frame)
        s.settimeout(args.listen)
        end = time.time() + args.listen
        while time.time() < end:
            try:
                data = s.recv(256)
                if not data:
                    break
                print(f"recv:    {hexdump(data)}  | {data!r}")
            except socket.timeout:
                break
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
