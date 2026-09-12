# gpb_client

Python client for the Raspberry Pi gamepad bridge. Standard library only — submoduling this
repo adds no dependencies to yours.

```python
from gpb_client import ControlClient, CaptureReceiver, GamepadState, Button

# Drive a console attached to the Pi at 192.168.1.50
with ControlClient("192.168.1.50", key="change-me") as pad:
    pad.tap(Button.EAST)                       # press and release
    pad.set_axes(lx=32767)                     # stick hard right
    pad.state.press(Button.L1)                 # or edit state directly

# Receive every state the bridge produced, for aligning against video captured here
with CaptureReceiver(port=9872, key="change-me") as cap:
    for state, arrived_ns in cap:
        ...
```

## Two things worth knowing

**The client streams continuously by default**, at 125 Hz. That is not redundancy for its own
sake: UDP loses datagrams, and a client that only sends on change loses the *event* — a
"button down" that goes missing never happens at all, and nothing on screen explains why.
Streaming means any loss is corrected within 8 ms.

This is the same failure the console taught this project directly: with change-only
reporting, analog sticks looked perfect while buttons were mostly ignored, because an axis
value is absolute and survives a gap where a press does not.

**Buttons are named by physical position**, not by vendor labels — `SOUTH`, `EAST`, `NORTH`,
`WEST`, following evdev's convention. Which letter sits at which position differs between
pads, and translating position to label is the bridge's job.

## Transports

| | |
|---|---|
| `ControlClient(host, port)` | UDP to the Pi |
| `ControlClient(unix_path=...)` | Unix socket, when running on the Pi itself |
| `CaptureReceiver(port)` | UDP from the Pi |

Authentication is HMAC-SHA256 appended to each datagram. Pass the same `key` the bridge
config uses; omit it on both sides to disable.
