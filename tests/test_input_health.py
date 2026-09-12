#!/usr/bin/env python3
"""Panel health assessments: input rejection, and capture loss.

Every network misconfiguration in this project's history failed the same way -- the service
healthy, the client apparently sending, nothing happening. A wrong key, a peer address that
did not match, a sequence number from a restarted client. The bridge knew every time and had
nowhere to say it.

These tests pin the distinction that makes the diagnosis possible: accepting NOTHING while
rejecting something is a misconfiguration; rejections alongside accepted traffic are normal.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "web"))
from gpb_web import assess_capture, assess_input  # noqa: E402

ACTIVE = {"active": "active"}
STOPPED = {"active": "inactive"}

failures = 0


def check(name, ok, detail=""):
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok:
        failures += 1
        if detail:
            print(f"        {detail}")


def counters(**kw):
    base = {"accepted": 0, "stale": 0, "bad_tag": 0, "bad_frame": 0, "wrong_peer": 0}
    base.update(kw)
    return {"source_counters": base}


# The three real incidents, each of which presented as silence.
r = assess_input(ACTIVE, counters(bad_tag=47), "udp")
check("a wrong key is named, not left as silence",
      r["level"] == "bad" and "authentication" in r["headline"] and "47" in r["detail"], repr(r))

r = assess_input(ACTIVE, counters(wrong_peer=12), "udp")
check("an unexpected sender address is named",
      r["level"] == "bad" and "address" in r["headline"], repr(r))

r = assess_input(ACTIVE, counters(stale=300), "udp")
check("datagrams rejected as stale are named",
      r["level"] == "bad" and "sequence" in r["headline"], repr(r))

# Rejections ALONGSIDE accepted traffic are ordinary: reordering happens, probes happen.
r = assess_input(ACTIVE, counters(accepted=9000, stale=3), "udp")
check("rejections alongside accepted input are reported without alarm",
      r["level"] == "ok" and "3" in r["detail"], repr(r))

# Silence with no rejections is not a fault -- nothing has been sent.
r = assess_input(ACTIVE, counters(), "udp")
check("no traffic at all is idle, not an error", r["level"] == "idle", repr(r))

# The dominant reason is the one worth reporting when several are non-zero.
r = assess_input(ACTIVE, counters(bad_tag=2, wrong_peer=500), "udp")
check("the dominant rejection reason is the one surfaced",
      "address" in r["headline"], repr(r))

# A controller source has nothing to reject; connectedness is the whole story.
check("a connected controller is ok",
      assess_input(ACTIVE, {"source_connected": True}, "evdev")["level"] == "ok")
check("a missing controller is idle, not an error",
      assess_input(ACTIVE, {"source_connected": False}, "evdev")["level"] == "idle")
check("a stopped bridge is idle regardless of counters",
      assess_input(STOPPED, counters(bad_tag=99), "udp")["level"] == "idle")


# --------------------------------------------------------------- capture
#
# The recorder's ring is bounded deliberately: a stalled disk must never block live input,
# so overflow discards training samples instead. That is silent data loss and has to be
# loud. Published capture is different -- UDP sendto succeeds whether or not anyone is
# listening, so the panel must not imply delivery it cannot observe.

def capture(**kw):
    base = {"recording": False, "written": 0, "dropped": 0,
            "publishing": False, "sent": 0, "failed": 0}
    base.update(kw)
    return {"capture": base}


r = assess_capture(ACTIVE, capture(recording=True, written=100, dropped=7))
check("dropped capture samples are reported as a fault",
      r["level"] == "bad" and "7" in r["detail"], repr(r))

r = assess_capture(ACTIVE, capture(recording=True, written=5000))
check("healthy recording is ok", r["level"] == "ok" and "5000" in r["detail"], repr(r))

r = assess_capture(ACTIVE, capture(publishing=True, sent=900, failed=4))
check("local send errors are a warning, not a fault",
      r["level"] == "warn" and "4" in r["detail"], repr(r))

# The wording matters: sendto succeeding is not evidence anything arrived.
r = assess_capture(ACTIVE, capture(publishing=True, sent=900))
check("publishing claims datagrams sent, never delivered",
      r["level"] == "ok" and "delivered" not in r["detail"].lower()
      and "receiver" in r["detail"].lower(), repr(r))

check("capture is hidden when neither recording nor publishing",
      assess_capture(ACTIVE, capture())["level"] == "off")
check("capture is hidden when the bridge is stopped",
      assess_capture(STOPPED, capture(recording=True, dropped=99))["level"] == "off")

sys.exit(1 if failures else 0)
