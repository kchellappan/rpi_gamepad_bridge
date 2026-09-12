"""Datagram authentication.

The tag is appended to the 48-byte state rather than embedded in it, so the state itself
stays byte-identical to what the bridge records and replays. An unauthenticated deployment
sends exactly the same datagram without the suffix.
"""
from __future__ import annotations

import hashlib
import hmac

TAG_BYTES = 16   # HMAC-SHA256, truncated


def sign(key: str | bytes | None, payload: bytes) -> bytes:
    if not key:
        return payload
    k = key.encode() if isinstance(key, str) else key
    return payload + hmac.new(k, payload, hashlib.sha256).digest()[:TAG_BYTES]


def verify(key: str | bytes | None, datagram: bytes, body_len: int) -> bytes | None:
    """Returns the state bytes if authentic, else None."""
    if not key:
        return datagram[:body_len] if len(datagram) == body_len else None
    if len(datagram) != body_len + TAG_BYTES:
        return None
    k = key.encode() if isinstance(key, str) else key
    want = hmac.new(k, datagram[:body_len], hashlib.sha256).digest()[:TAG_BYTES]
    # compare_digest: a plain == leaks how much of the tag matched, via timing.
    if not hmac.compare_digest(want, datagram[body_len:]):
        return None
    return datagram[:body_len]
