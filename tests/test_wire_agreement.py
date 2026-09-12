#!/usr/bin/env python3
"""The C++ and Python definitions of GamepadState must agree byte for byte.

Agreement on sizeof() is not agreement on layout. Two definitions can be the same size with
fields transposed or padding misplaced, and the size check would pass while every value on
the wire was wrong -- silently, in both directions at once, for captures and control alike.

So this compares actual bytes for states chosen to make a transposition impossible to miss.
"""
from __future__ import annotations

import pathlib
import subprocess
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "clients" / "python"))
from gpb_client.state import FORMAT, MAGIC, SIZE, VERSION, GamepadState  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "gpb-wire-test")

CASES = [
    dict(seq=0, buttons=0, lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, t_mono_ns=0, t_real_ns=0),
    # Every field a distinct value: a transposition cannot look like a match.
    dict(seq=1, buttons=0, lx=1, ly=2, rx=3, ry=4, lt=5, rt=6, t_mono_ns=7, t_real_ns=8),
    # Extremes and negatives, where a sign or width error shows up.
    dict(seq=0xDEADBEEF, buttons=0x0003FFFF, lx=-32767, ly=32767, rx=-1, ry=1, lt=255,
         rt=254, t_mono_ns=0x0123456789ABCDEF, t_real_ns=0xFEDCBA9876543210),
    # Lowest and highest button bits, so the bitfield's width is covered.
    dict(seq=42, buttons=(1 << 0) | (1 << 17), lx=0, ly=0, rx=0, ry=0, lt=0, rt=0,
         t_mono_ns=1, t_real_ns=2),
]


def python_bytes(case: dict) -> str:
    if case["t_mono_ns"] == 0:
        # pack() substitutes the current time for a zero timestamp, which is right for real
        # use and unusable for a fixed comparison, so build this one literally.
        return struct.pack(FORMAT, MAGIC, VERSION, SIZE, case["seq"], case["buttons"],
                           0, 0, case["lx"], case["ly"], case["rx"], case["ry"],
                           case["lt"], case["rt"], b"\0" * 6).hex()
    return GamepadState(**case).pack().hex()


def main() -> int:
    try:
        out = subprocess.run([BIN, "states"], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError) as e:
        print(f"  FAIL  could not run {BIN}: {e}")
        return 1
    cpp = out.stdout.split()
    if len(cpp) != len(CASES):
        print(f"  FAIL  expected {len(CASES)} states from the C++ side, got {len(cpp)}")
        return 1

    failures = 0
    for i, (case, want) in enumerate(zip(CASES, cpp)):
        got = python_bytes(case)
        if got == want:
            print(f"  PASS  wire layout case {i} is byte-identical in C++ and Python")
        else:
            print(f"  FAIL  wire layout case {i} differs between C++ and Python")
            print(f"        c++    {want}")
            print(f"        python {got}")
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
