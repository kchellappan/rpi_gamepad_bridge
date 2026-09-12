# gpb_client (C++)

C++ client for the gamepad bridge. No dependencies beyond the standard library, so
submoduling this repo adds two source files to your build and nothing else.

```cpp
#include "gpb_client/client.hpp"

// Ask what you are driving before you drive it.
auto caps = gpb::client::query_capabilities("192.168.1.50", 9871);
// caps.target        -> "HORIPAD for Nintendo Switch"
// caps.trigger_mode  -> "digital"

gpb::client::ControlClient::Options opts;
opts.host = "192.168.1.50";
opts.key  = "change-me";
gpb::client::ControlClient pad(opts);

std::string err;
pad.connect(err);
pad.tap(gpb::btn::kEast);
pad.set_axes(32767, 0, 0, 0);

if (caps.analog_triggers()) pad.set_triggers(255, 0);
else                        pad.press(gpb::btn::kL2);
```

Receiving capture, for aligning against video captured on this machine:

```cpp
gpb::client::CaptureReceiver cap(9872, "0.0.0.0", "change-me");
std::string err;
cap.open(err);

gpb::GamepadState s;
while (cap.recv(s, 1000)) { /* s.t_mono_ns, s.t_real_ns are the Pi's clocks */ }
```

## Why ask about capabilities

Because guessing fails silently. The HORIPAD's ZL/ZR are **buttons**, not analog triggers. A
client that streams `lt`/`rt` because its own controller has analog triggers used to produce
nothing at all on that target — no error, no clue. A PC target wants the opposite.

The bridge now accepts either representation wherever it reasonably can, so `trigger_mode`
is there to let you send the *right* thing rather than to reject the other one.

## Streaming

`ControlClient` streams continuously at 125 Hz by default rather than sending only on
change, and that default is load-bearing. UDP drops datagrams, and a change-only client
loses the **event**: a "button down" that goes missing never happens at all.

This is the same failure the console taught this project directly — with change-only
reporting, analog sticks looked perfect while buttons were mostly ignored, because an axis
value is absolute and survives a gap where a press does not.

## Building

CMake targets `gpb_client` (static library) and `gpb-drive-example`. Or compile directly:

```
g++ -std=c++20 -Iinclude -Iclients/cpp/include your.cpp \
    clients/cpp/src/client.cpp src/wire.cpp -lpthread
```
