"""Driving the gamepad: your machine -> the Pi."""
from __future__ import annotations

import socket
import threading
import time

from .auth import sign
from .state import SIZE, GamepadState


class ControlClient:
    """Sends controller state to the bridge.

    Streams continuously by default, and that default matters. UDP loses datagrams, and a
    client that only sends on change loses the *event*: a "button down" that goes missing
    never happens at all, while the user sees nothing wrong. Streaming at 125 Hz means any
    loss is corrected within 8 ms by the next datagram.

    It is the same lesson the console taught this project the hard way -- with change-only
    reporting, analog sticks looked perfect while buttons were mostly ignored, because an
    axis value is absolute and survives a gap where a press does not.

        with ControlClient("192.168.1.50") as pad:
            pad.press(Button.EAST)
            time.sleep(0.1)
            pad.release(Button.EAST)
    """

    def __init__(self, host: str | None = None, port: int = 9871, *,
                 unix_path: str | None = None, key: str | None = None,
                 rate_hz: float = 125.0, stream: bool = True):
        if not host and not unix_path:
            raise ValueError("give either host or unix_path")
        self.key = key
        self.state = GamepadState()
        self._seq = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._interval = 1.0 / rate_hz if rate_hz > 0 else 0.008

        if unix_path:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            self._sock.connect(unix_path)
            self._dest = None
        else:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._dest = (host, port)

        if stream:
            self._thread = threading.Thread(target=self._run, daemon=True)
            self._thread.start()

    # -- lifecycle
    def __enter__(self) -> "ControlClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)
        # Release everything on the way out. The bridge holds the last state it received, so
        # exiting mid-press would leave that button held on the console indefinitely.
        try:
            self.state = GamepadState()
            self.send()
        except OSError:
            pass
        self._sock.close()

    # -- sending
    def send(self, state: GamepadState | None = None) -> None:
        """Send one datagram immediately."""
        with self._lock:
            if state is not None:
                self.state = state
            self._seq = (self._seq + 1) & 0xFFFFFFFF
            payload = sign(self.key, self.state.pack(seq=self._seq))
        if self._dest:
            self._sock.sendto(payload, self._dest)
        else:
            self._sock.send(payload)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self.send()
            except OSError:
                pass   # a transient network error must not kill the stream
            time.sleep(self._interval)

    # -- convenience
    def press(self, button) -> None:
        with self._lock:
            self.state.press(button)
        self.send()

    def release(self, button) -> None:
        with self._lock:
            self.state.release(button)
        self.send()

    def tap(self, button, hold_s: float = 0.05) -> None:
        """Press and release, holding long enough for the console to notice.

        The default is not arbitrary: the gadget's endpoint is polled once per millisecond,
        but a console samples across several polls before it accepts a press as real. A tap
        shorter than a few tens of milliseconds is unreliable on real hardware.
        """
        self.press(button)
        time.sleep(hold_s)
        self.release(button)

    def set_axes(self, *, lx=None, ly=None, rx=None, ry=None, lt=None, rt=None) -> None:
        with self._lock:
            for name, value in (("lx", lx), ("ly", ly), ("rx", rx),
                                ("ry", ry), ("lt", lt), ("rt", rt)):
                if value is not None:
                    setattr(self.state, name, value)
        self.send()
