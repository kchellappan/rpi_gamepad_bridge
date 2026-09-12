#!/usr/bin/env python3
"""UDP transport: authentication, staleness, peer restriction, and both directions.

These are the paths where being wrong is expensive rather than annoying. The control port
injects controller input into whatever console the gadget is plugged into, so "rejects a
forged datagram" is a claim that should be tested rather than asserted.
"""
from __future__ import annotations

import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "clients" / "python"))
from gpb_client import Button, CaptureReceiver, ControlClient, GamepadState  # noqa: E402
from gpb_client.auth import sign  # noqa: E402
from gpb_client.state import SIZE  # noqa: E402

KEY = "test-key"
failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok:
        failures += 1
        if detail:
            print(f"        {detail}")


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_bridge(tmp, ctrl_port, pub_port, key=KEY, peer="127.0.0.1"):
    hid = tmp / "hid.bin"
    hid.write_bytes(b"")
    cfg = tmp / "udp.ini"
    cfg.write_text(f"""
[bridge]
source = udp
sink = ns_hid
publish_host = 127.0.0.1
publish_port = {pub_port}
publish_key = {key}
[source.udp]
bind = 127.0.0.1
port = {ctrl_port}
peer = {peer}
key = {key}
[sink.ns_hid]
device = {hid}
heartbeat_hz = 0
""")
    proc = subprocess.Popen([str(ROOT / "build" / "gpbridge"), "--config", str(cfg)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    return proc, hid


def reports(path):
    d = path.read_bytes()
    return [d[i:i + 8] for i in range(0, len(d) // 8 * 8, 8)]


with tempfile.TemporaryDirectory() as td:
    tmp = pathlib.Path(td)
    ctrl, pub = free_port(), free_port()
    bridge, hid = start_bridge(tmp, ctrl, pub)
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dest = ("127.0.0.1", ctrl)

        # A correctly signed datagram is applied.
        sock.sendto(sign(KEY, GamepadState(lx=32767).press(Button.EAST).pack(seq=10)), dest)
        time.sleep(0.3)
        before = len(reports(hid))
        check("a signed datagram is applied", before >= 2, f"{before} reports")

        # Forged tag: the same state signed with the wrong key must be ignored.
        sock.sendto(sign("wrong-key", GamepadState().press(Button.NORTH).pack(seq=11)), dest)
        time.sleep(0.3)
        check("a datagram signed with the wrong key is rejected",
              len(reports(hid)) == before, "state changed despite a bad tag")

        # Unsigned datagram, when a key is configured.
        sock.sendto(GamepadState().press(Button.WEST).pack(seq=12), dest)
        time.sleep(0.3)
        check("an unsigned datagram is rejected when a key is set",
              len(reports(hid)) == before)

        # Stale sequence: an older seq has already been superseded and must not be applied.
        sock.sendto(sign(KEY, GamepadState().press(Button.SOUTH).pack(seq=5)), dest)
        time.sleep(0.3)
        check("an out-of-order (stale) datagram is rejected",
              len(reports(hid)) == before, "a late datagram moved the controller backwards")

        # A newer one still works, so the staleness rule has not wedged the link.
        sock.sendto(sign(KEY, GamepadState().press(Button.SOUTH).pack(seq=20)), dest)
        time.sleep(0.3)
        check("a newer datagram is still accepted", len(reports(hid)) > before)

        # Capture flows back out.
        got = []
        with CaptureReceiver(port=pub, bind="127.0.0.1", key=KEY, timeout=2.0) as cap:
            sock.sendto(sign(KEY, GamepadState(ly=-32767).press(Button.R1).pack(seq=30)), dest)
            try:
                item = cap.recv()
                if item:
                    got.append(item[0])
            except OSError:
                pass
        check("capture is published back to the client",
              bool(got) and got[0].is_pressed(Button.R1),
              f"received {got}")

        # Capture verified with the wrong key must be rejected, not silently trusted.
        with CaptureReceiver(port=free_port(), bind="127.0.0.1", key=KEY, timeout=0.2) as cap:
            raw = sign("nope", GamepadState().pack(seq=1))
            s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s2.sendto(raw, ("127.0.0.1", cap._sock.getsockname()[1]))
            try:
                check("a capture datagram with a bad tag is rejected", cap.recv() is None)
            except OSError:
                check("a capture datagram with a bad tag is rejected", False, "timed out")
    finally:
        bridge.terminate()
        bridge.wait(timeout=5)

# Capabilities: a client cannot know whether a target's triggers are analog or buttons, and
# guessing wrong fails silently. Asking has to work.
with tempfile.TemporaryDirectory() as td:
    tmp = pathlib.Path(td)
    ctrl, pub = free_port(), free_port()
    bridge, hid = start_bridge(tmp, ctrl, pub, key="", peer="")
    try:
        q = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        q.settimeout(2.0)
        q.sendto(b"GPBQCAPS", ("127.0.0.1", ctrl))
        import json as _json
        try:
            caps = _json.loads(q.recv(4096).decode())
        except (OSError, ValueError) as e:
            caps = {}
            check("the bridge answers a capability query", False, str(e))
        if caps:
            check("the bridge answers a capability query", True)
            check("capabilities name the target and its trigger style",
                  caps.get("target") and caps.get("trigger_mode") in ("digital", "analog"),
                  repr(caps))
            check("the HORIPAD advertises digital triggers",
                  caps.get("trigger_mode") == "digital", repr(caps.get("trigger_mode")))

        # The regression this whole mechanism exists around: a client streaming ANALOG
        # triggers produced nothing at all on a target whose ZL/ZR are buttons, because the
        # analog-to-digital shadow lives in a transform that canonical sources skip.
        before = len(reports(hid))
        q.sendto(GamepadState(lt=255, rt=255).pack(seq=100), ("127.0.0.1", ctrl))
        time.sleep(0.3)
        after = reports(hid)
        zl_zr = any((r[0] & 0x40) and (r[0] & 0x80) for r in after[before:])
        check("analog trigger values reach a target whose triggers are buttons", zl_zr,
              "reports: " + " ".join(r.hex() for r in after[before:]))
    finally:
        bridge.terminate()
        bridge.wait(timeout=5)

# The Python and C++ definitions of the state must not drift: a silent divergence would
# corrupt every capture and every injected input at once, while looking entirely healthy.
header = (ROOT / "include" / "gpb" / "gamepad_state.hpp").read_text()
check("the python wire format matches the C++ static_assert",
      f"sizeof(GamepadState) == {SIZE}" in header,
      f"python says {SIZE}; the header asserts something else")

sys.exit(1 if failures else 0)
