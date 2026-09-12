"""Asking the bridge what it is driving.

A client cannot know whether a target's triggers are analog or buttons, which face buttons
exist, or what the thing on the other end even is. Guessing wrong does not raise -- it
produces a controller that quietly does nothing, which is the hardest kind of problem to
chase from the client side.

So ask:

    caps = query_capabilities("192.168.1.50")
    if caps.analog_triggers:
        pad.state.lt = 255
    else:
        pad.state.press(Button.L2)

The query is unauthenticated by design. It reveals only what kind of controller the bridge
presents, which is not a secret, and requiring a key to ask would leave a client with the
wrong key unable to discover that fact.
"""
from __future__ import annotations

import json
import socket
from dataclasses import dataclass, field

QUERY = b"GPBQCAPS"


@dataclass
class Capabilities:
    ok: bool = False
    sink: str = ""
    target: str = ""
    trigger_mode: str = ""
    axes: list[str] = field(default_factory=list)
    buttons: list[str] = field(default_factory=list)
    notes: str = ""
    raw: dict = field(default_factory=dict)

    @property
    def analog_triggers(self) -> bool:
        """True when the target has real analog triggers, so lt/rt is the direct form."""
        return self.trigger_mode == "analog"

    def supports_button(self, name: str) -> bool:
        """Whether the target has this control at all.

        A DualSense mapping has no Capture button to give, for instance -- so a client
        scripting one should know before it tries.
        """
        return name in self.buttons

    def __bool__(self) -> bool:
        return self.ok


def query_capabilities(host: str, port: int = 9871, timeout: float = 1.0) -> Capabilities:
    """Ask a bridge what it presents. Returns a falsy Capabilities if it does not answer."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(QUERY, (host, port))
        data, _ = s.recvfrom(4096)
    except OSError:
        return Capabilities()
    finally:
        s.close()

    try:
        doc = json.loads(data.decode("utf-8", "replace"))
    except ValueError:
        return Capabilities()

    return Capabilities(
        ok=bool(doc.get("target")),
        sink=doc.get("sink", ""),
        target=doc.get("target", ""),
        trigger_mode=doc.get("trigger_mode", ""),
        axes=list(doc.get("axes", [])),
        buttons=list(doc.get("buttons", [])),
        notes=doc.get("notes", ""),
        raw=doc,
    )
