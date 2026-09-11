#!/usr/bin/env python3
"""Inject GamepadState frames into a running bridge over its Unix socket.

The wire format is the same 48-byte struct the recorder writes, so this doubles as the
replay client. Kept dependency-free so CI needs nothing but python3.
"""
import socket
import struct
import sys
import time

FMT = "<IHHIIQQhhhhBB6s"
MAGIC, VERSION = 0x52474231, 1
assert struct.calcsize(FMT) == 48, struct.calcsize(FMT)

SOUTH, EAST, WEST, NORTH = 1 << 0, 1 << 1, 1 << 2, 1 << 3
DUP, DDOWN, DLEFT, DRIGHT = 1 << 14, 1 << 15, 1 << 16, 1 << 17


def frame(buttons=0, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0):
    return struct.pack(FMT, MAGIC, VERSION, 48, 0, buttons, 0, 0,
                       lx, ly, rx, ry, lt, rt, b"\0" * 6)


def connect(path, timeout=5.0):
    deadline = time.time() + timeout
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() > deadline:
                raise
            time.sleep(0.05)


def main():
    path, mode = sys.argv[1], sys.argv[2]
    s = connect(path)
    time.sleep(0.3)   # let the bridge register the client before the first frame

    if mode == "encode":
        # Chosen to exercise face buttons, a full-scale axis, and hat diagonals at once.
        frames = [
            frame(buttons=EAST, lx=32767),
            frame(buttons=NORTH | DUP | DLEFT),
            frame(),
        ]
    elif mode == "replay":
        blob = open(sys.argv[3], "rb").read()
        frames = [blob[i:i + 48] for i in range(0, len(blob) // 48 * 48, 48)]
    else:
        raise SystemExit(f"unknown mode {mode}")

    for f in frames:
        s.send(f)
        # Space the frames out: without a heartbeat the bridge writes one report per state,
        # and the sink must have drained each before the next arrives or coalescing will
        # (correctly) discard the older one and the byte comparison becomes timing-dependent.
        time.sleep(0.2)
    time.sleep(0.3)
    s.close()
    print(f"sent {len(frames)} frames")


if __name__ == "__main__":
    main()
