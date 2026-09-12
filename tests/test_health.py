#!/usr/bin/env python3
"""Unit tests for the control panel's link assessment.

This is the decision the panel got wrong on real hardware: /sys/class/udc reported
state=configured at high-speed while the gadget's interrupt endpoint was disabled and every
write failed. The page showed a healthy connection for the entire outage, which is worse
than showing nothing, because it directs attention away from the actual fault.

The rule these tests encode: "attached" is not "working". Saying it is working requires
evidence from the bridge that reports are landing.
"""
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "web"))
from gpb_web import assess  # noqa: E402

ACTIVE = {"active": "active"}
STOPPED = {"active": "inactive"}
ATTACHED = {"present": True, "state": "configured", "speed": "high-speed"}
DETACHED = {"present": True, "state": "not attached", "speed": "UNKNOWN"}
NO_UDC = {"present": False}

HEALTHY = {"ever_wrote": True, "write_failures": 0, "since_write_ok_ms": 8, "submits": 900}
# The real failure: link says connected, nothing has ever reached it.
WEDGED = {"ever_wrote": False, "write_failures": 4000, "since_write_ok_ms": 0, "submits": 4000}
# Was working, then stopped.
STALLED = {"ever_wrote": True, "write_failures": 900, "since_write_ok_ms": 12000, "submits": 5000}

cases = [
    ("healthy link is ok", ACTIVE, ATTACHED, HEALTHY, "ok"),
    ("attached but nothing ever sent is BAD", ACTIVE, ATTACHED, WEDGED, "bad"),
    ("was sending, now stalled is BAD", ACTIVE, ATTACHED, STALLED, "bad"),
    ("no console attached is idle, not an error", ACTIVE, DETACHED, {}, "idle"),
    ("bridge stopped is idle", STOPPED, ATTACHED, HEALTHY, "idle"),
    ("no gadget at all is bad", ACTIVE, NO_UDC, {}, "bad"),
    ("no health yet is unknown, not ok", ACTIVE, ATTACHED, {}, "unknown"),
]

failures = 0
for name, bridge, udc, stats, expected in cases:
    got = assess(bridge, udc, stats)
    if got.get("level") == expected:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}")
        print(f"        expected level={expected!r}, got {got!r}")
        failures += 1

# A loopback into this Pi's own USB-A port fails writes whenever nothing is reading the
# gadget, because usbhid only polls while a client has the node open. That is normal, and
# calling it a wedged console sends the user to re-enumerate something that works.
loop = assess(ACTIVE, ATTACHED, WEDGED, loopback=True)
if loop["level"] == "idle" and "loop" in loop["headline"].lower():
    print("  PASS  a loopback with no reader is reported as normal, not as a fault")
else:
    print(f"  FAIL  loopback misreported: {loop!r}")
    failures += 1

# ...but the same symptoms WITHOUT a loopback are still a genuine fault.
if assess(ACTIVE, ATTACHED, WEDGED, loopback=False)["level"] == "bad":
    print("  PASS  the same symptoms without a loopback remain a fault")
else:
    print("  FAIL  loopback handling swallowed a real fault")
    failures += 1

# The specific regression, stated plainly: a configured link with dead writes must never be
# reported as fine.
verdict = assess(ACTIVE, ATTACHED, WEDGED)
if verdict["level"] == "bad" and "not accepting" in verdict["headline"].lower():
    print("  PASS  a configured link with dead writes is never reported as connected")
else:
    print(f"  FAIL  wedged link was not surfaced clearly: {verdict!r}")
    failures += 1

sys.exit(1 if failures else 0)
