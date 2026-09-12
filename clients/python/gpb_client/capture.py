"""Receiving capture: the Pi -> your machine."""
from __future__ import annotations

import socket

from .auth import TAG_BYTES, verify
from .state import SIZE, GamepadState


class CaptureReceiver:
    """Receives every state the bridge produced, as it happens.

    The point of taking this over the network rather than reading the Pi's recording is
    clock alignment: if video is being captured on this machine, timestamping both streams
    here avoids reconciling two machines' clocks afterwards.

    Each state still carries the Pi's own CLOCK_MONOTONIC and CLOCK_REALTIME values, so the
    Pi-side ordering is preserved and available if needed.

        with CaptureReceiver(port=9872) as cap:
            for state, received_ns in cap:
                align_with_video(state, received_ns)
    """

    def __init__(self, port: int = 9872, bind: str = "0.0.0.0", *,
                 key: str | None = None, timeout: float | None = None):
        self.key = key
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((bind, port))
        if timeout is not None:
            self._sock.settimeout(timeout)
        self.rejected = 0

    def __enter__(self) -> "CaptureReceiver":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self._sock.close()

    def recv(self) -> tuple[GamepadState, int] | None:
        """One state plus the local arrival time in nanoseconds, or None if it was rejected."""
        import time
        data, _ = self._sock.recvfrom(SIZE + TAG_BYTES)
        arrived = time.monotonic_ns()
        body = verify(self.key, data, SIZE)
        if body is None:
            self.rejected += 1
            return None
        try:
            return GamepadState.unpack(body), arrived
        except ValueError:
            self.rejected += 1
            return None

    def __iter__(self):
        while True:
            item = self.recv()
            if item is not None:
                yield item
