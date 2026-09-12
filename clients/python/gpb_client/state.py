"""The wire format, and the one place it is defined for Python.

The layout must match struct GamepadState in include/gpb/gamepad_state.hpp exactly. The test
suite asserts the two agree on size, because a silent divergence here would corrupt every
capture and every injected input at once while looking perfectly healthy.
"""
from __future__ import annotations

import enum
import struct
import time
from dataclasses import dataclass

# uint32 magic, uint16 version, uint16 size, uint32 seq, uint32 buttons,
# uint64 t_mono_ns, uint64 t_real_ns, 4x int16 axes, 2x uint8 triggers, 6 pad
FORMAT = "<IHHIIQQhhhhBB6s"
SIZE = struct.calcsize(FORMAT)
assert SIZE == 48, SIZE

MAGIC = 0x52474231   # "RGB1", from before the project was renamed
VERSION = 1


class Button(enum.IntFlag):
    """Named by physical position, following evdev's BTN_SOUTH/EAST/NORTH/WEST convention.

    Deliberately not by vendor labels: which letter sits at which position differs between
    pads, and translating position to label is the sink's job, not the client's.
    """

    SOUTH = 1 << 0      # Xbox A   / PS cross    / Switch B
    EAST = 1 << 1       # Xbox B   / PS circle   / Switch A
    WEST = 1 << 2       # Xbox X   / PS square   / Switch Y
    NORTH = 1 << 3      # Xbox Y   / PS triangle / Switch X
    L1 = 1 << 4
    R1 = 1 << 5
    L2 = 1 << 6
    R2 = 1 << 7
    SELECT = 1 << 8     # Switch Minus
    START = 1 << 9      # Switch Plus
    L3 = 1 << 10
    R3 = 1 << 11
    GUIDE = 1 << 12     # Switch Home
    MISC1 = 1 << 13     # Capture / Share
    DPAD_UP = 1 << 14
    DPAD_DOWN = 1 << 15
    DPAD_LEFT = 1 << 16
    DPAD_RIGHT = 1 << 17


@dataclass
class GamepadState:
    """One controller state.

    Axis convention is +x right, +y DOWN. Down-positive matches both evdev and USB HID, so a
    value passes end to end without an inversion at either boundary -- and every inversion is
    a chance to get the sign wrong.
    """

    buttons: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0
    lt: int = 0
    rt: int = 0
    seq: int = 0
    t_mono_ns: int = 0
    t_real_ns: int = 0

    def press(self, button: Button) -> "GamepadState":
        self.buttons |= int(button)
        return self

    def release(self, button: Button) -> "GamepadState":
        self.buttons &= ~int(button)
        return self

    def is_pressed(self, button: Button) -> bool:
        return bool(self.buttons & int(button))

    def pack(self, seq: int | None = None) -> bytes:
        if seq is not None:
            self.seq = seq
        return struct.pack(
            FORMAT, MAGIC, VERSION, SIZE, self.seq & 0xFFFFFFFF, self.buttons & 0xFFFFFFFF,
            self.t_mono_ns or time.monotonic_ns(),
            self.t_real_ns or time.time_ns(),
            _clamp16(self.lx), _clamp16(self.ly), _clamp16(self.rx), _clamp16(self.ry),
            self.lt & 0xFF, self.rt & 0xFF, b"\0" * 6,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "GamepadState":
        magic, version, size, seq, buttons, tmono, treal, lx, ly, rx, ry, lt, rt, _ = \
            struct.unpack(FORMAT, data[:SIZE])
        if magic != MAGIC or version != VERSION:
            raise ValueError(f"not a GamepadState (magic={magic:#x} version={version})")
        return cls(buttons=buttons, lx=lx, ly=ly, rx=rx, ry=ry, lt=lt, rt=rt,
                   seq=seq, t_mono_ns=tmono, t_real_ns=treal)


def _clamp16(v: int) -> int:
    return max(-32767, min(32767, int(v)))
