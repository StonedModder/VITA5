#!/usr/bin/env python3
# VITA5 - ps5-payload-dev/elfldr sender + console capture.
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Send a payload ELF to elfldr over its documented raw-TCP interface
(the `nc -q0 $PS5_HOST 9021 < payload.elf` protocol), then keep the
socket open to capture the payload's console output.

  python3 tools/payload_send.py payload/VITA5-touchswipe.elf
  python3 tools/payload_send.py --host "$PS5_HOST" --port 9021 --wait 45 file.elf

elfldr accepts either a raw ELF stream (this tool), or a one-line URI
(file:/..., https://...) - see ps5-payload-dev/elfldr README.
"""
import argparse
import socket
import sys
import time

DEFAULT_HOST = os.environ.get("PS5_HOST", "")
DEFAULT_PORT = 9021


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", help="payload ELF to send")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--wait", type=float, default=45.0,
                    help="seconds to capture console output after upload")
    args = ap.parse_args()
    if not args.host:
        print("ERROR: pass --host or set PS5_HOST", file=sys.stderr)
        return 2

    with open(args.elf, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF":
        print(f"ERROR: {args.elf} is not an ELF", file=sys.stderr)
        return 2

    print(f"[*] {len(elf)} bytes -> {args.host}:{args.port}")
    s = socket.socket()
    s.settimeout(15)
    s.connect((args.host, args.port))
    s.sendall(elf)
    s.shutdown(socket.SHUT_WR)  # elfldr reads the stream until EOF

    print(f"[*] sent; capturing console for {args.wait:.0f}s")
    s.settimeout(2)
    start = time.time()
    out = []
    while time.time() - start < args.wait:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            out.append(chunk)
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
        except socket.timeout:
            continue
    s.close()
    text = b"".join(out).decode("utf-8", "replace")
    print(f"\n[*] done: {len(text)} bytes of console output")
    return 0 if text else 3


if __name__ == "__main__":
    sys.exit(main())
