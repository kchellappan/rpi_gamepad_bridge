# rpi_gamepad_bridge

A low-latency bridge that reads from an arbitrary input peripheral (a wired gamepad, a
programmatic client, a CAN joystick) and re-emits it as a USB gamepad that a console or PC
accepts as a real controller.

**Purpose:** capture human gameplay demonstrations for training a VLA (vision-language-action)
model, then drive the same console from that model's inferred actions at runtime. Capture and
inject are therefore two directions of one pipeline, and they must agree bit-for-bit.

Spiritual successor to prior work forked from
[gdsports/NSGadget_Pi](https://github.com/gdsports/NSGadget_Pi), rewritten from scratch with
a generic, source-agnostic / sink-agnostic architecture.

---

## Hardware

### Raspberry Pi 5

The Pi 5 is the target board. Its USB-C port is wired to a USB 2.0 controller (`dwc2`) that
can run in **peripheral (gadget) mode**, which is what lets the Pi masquerade as a USB HID
gamepad to a host. Note that the USB-C port is **USB 2.0 data only** — the USB 3 pins are
not connected — which is fine here, since HID gamepad traffic is tiny.

Enabling gadget mode (to be re-verified on the actual board):

```bash
# /boot/firmware/config.txt
dtoverlay=dwc2,dr_mode=peripheral
```

The overlay pulls in the module itself, so `modules-load=dwc2` in `cmdline.txt` should be
*omitted* to avoid conflicting with it.

### USB-C OTG power/data splitter cable -- validated

<https://www.amazon.com/Charging-Adapter-Splitter-Compatible-Chromecast/dp/B0B5MPCJF5/>

**Why it is required:** the Pi 5 has a single USB-C port, and it is both the power input and
the only OTG-capable data port. You cannot simultaneously power the Pi from that port and
use it as a USB device with a plain cable. The splitter breaks the port out into a separate
power leg and a separate data leg, so the Pi stays powered from its own supply while the
data leg goes to the host being controlled.

This cable is known good: it has driven this exact board successfully before, on a
Bookworm image flashed around April 2025.

### The data-leg cable matters, and a bad one is silent

Enumeration failed for a long time with this signature:

```
/sys/class/udc/*/state          = not attached      (forever)
/sys/class/udc/*/current_speed  = UNKNOWN
```

and nothing -- no attach, reset or suspend -- in `dmesg` on either side. **The cause was the
cable on the data leg.** Swapping it for a different one produced immediate enumeration at
high speed.

This failure gives you no diagnostic signal at all. A gadget that never sees VBUS stays
dormant, never asserts its D+ pull-up, and is indistinguishable from an empty port: the host
logs nothing, the device logs nothing, and every layer above looks healthy. `dwc2`'s
debugfs is the one place it shows:

```
DCTL=0x00000000    SftDiscon clear -- the driver is NOT holding the line down
DSTS=0x00000003    SuspSts set -- suspended, waiting for a session that never starts
```

If enumeration does not happen, change the cable before suspecting anything else.

### Note on kernel versions

There is an upstream report that on Trixie with kernel 6.18, `dwc2` never drives the D+/D-
lines. This project hit an identical-looking symptom on 6.18.34 and 6.18.39 and concluded
it had reproduced that regression. **That conclusion was wrong** -- those tests were run with
the faulty cable, which produces exactly the same silence. The regression has *not* been
reproduced here with known-good hardware.

Current verified configuration is Bookworm with kernel `6.6.51+rpt-rpi-2712` (held via
`apt-mark hold`). Whether 6.18 works with a good cable is untested; if you want to move to
Trixie, that is a single experiment rather than a known blocker.

### Input peripheral

The primary use case is a **wired Xbox controller** plugged into one of the Pi's USB-A
ports. The Pi is simultaneously a USB *host* (USB-A ports, driven by the RP1 southbridge)
and a USB *device* (USB-C port, driven by `dwc2`) — these are separate controllers, so
there is no conflict between reading the Xbox pad and presenting the gadget.

Other peripherals (e.g. CAN joysticks over a USB-CAN adapter) are explicitly *not* the
current target, but the source abstraction exists so that supporting one later is a new
class and a config entry rather than a rewrite.

Controllers available for this project, all of which reach userspace through evdev:

| Controller | Kernel driver | Notes |
|---|---|---|
| PDP Xbox (wired) | `xpad` | **Preferred primary.** Wired, mature driver, no BT jitter. |
| Switch Pro | `hid-nintendo` | USB and Bluetooth. |
| PS5 DualSense | `hid-playstation` | USB and Bluetooth. |
| Stadia | `hid-generic` | Enumerates as a standard USB HID gamepad. |

That all four land on evdev is the point: they cost one config table each, not one class each.

---

## Architecture (proposed)

Three layers, joined by a normalized intermediate state struct so that sources and sinks
never know about each other:

```
InputSource (abstract)          GamepadState              OutputSink (abstract)
  ├─ EvdevSource          ──►   (normalized,        ──►     ├─ NsHidGadgetSink   (Switch)
  ├─ SocketSource                POD, no alloc)             ├─ XInputGadgetSink  (X360)
  ├─ CanJoystickSource                                      └─ ...
  └─ ...                                                    
                    ▲                                  ▲
                    └────────── Bridge ────────────────┘
                     (owns one of each by composition,
                      applies the mapping profile)
```

### `SocketSource` — programmatic control

One first-class input mode is a socket that lets an external application drive the output
directly. This doubles as the test/replay harness and as the automation path (scripted
inputs, an agent driving the console, a remote UI).

Implementation preference: a **Unix domain socket** (`SOCK_SEQPACKET`, or `SOCK_DGRAM`)
rather than TCP loopback — message framing comes for free, there is no Nagle/delayed-ACK
interaction to defeat, and it skips the TCP/IP stack entirely. If a *remote* driver is ever
needed, that is a separate `UdpSource` rather than a reason to make the local path TCP.
The wire format should be a fixed-size POD struct matching `GamepadState`, so the read path
is a single `recv()` into the state with no parsing.

- `InputSource` — produces normalized `GamepadState` updates; event-driven off a pollable fd.
  `EvdevSource` is the workhorse: the kernel's evdev layer has already done the
  "normalize an arbitrary peripheral into axes and buttons" job, so one class covers the
  Xbox pad, PlayStation pads, and generic USB joysticks, with config mapping evdev codes
  onto `GamepadState` fields. Sources that cannot go through evdev (CAN, or a raw HID
  device we deliberately unbind from its driver) get their own class — that is the whole
  reason the abstraction exists.
- `OutputSink` — serializes `GamepadState` into the wire report for its target device.
- `Bridge` — owns one of each, applies the mapping/calibration profile, runs the hot loop.
- Factories for both base classes, keyed by strings from a config file, so a new source or
  sink is a single registration.

---

## Output targets: what is actually reachable

The `OutputSink` abstraction is worth building regardless, but the targets are not equally
achievable, and the blocker on two of them is cryptography rather than effort:

| Target | Feasible? | Why |
|---|---|---|
| **Nintendo Switch** | **Yes** | Present as a HORI Pokken Tournament Pro Pad. Plain USB HID, no authentication handshake. This is the validated path. |
| **PC (Linux / DirectInput)** | **Yes** | A generic HID gamepad descriptor is accepted directly. |
| **PC (Windows / XInput)** | **Yes, harder** | Requires impersonating an Xbox 360 pad — matching VID/PID `045E:028E` plus its vendor-specific interface descriptors — so Windows binds its inbox `xusb` driver. More fiddly than HID, but no crypto. |
| **PS4 / PS5** | **Blocked** | DualShock 4 / DualSense authenticate against the console via a challenge/response signed by a key held in a licensed auth IC. Unlicensed adapters work by proxying a genuine controller as an auth donor. Without that silicon the console rejects the pad (DS4 historically times out after ~8 minutes). |
| **Xbox One / Series** | **Blocked** | The GIP protocol carries a security handshake requiring a licensed chip. This is why every third-party Xbox pad is "Designed for Xbox" licensed. |

So: design the base class for all of them, but plan on **Switch and PC**. PlayStation and
Xbox consoles are not an engineering-effort problem, and no amount of clean architecture
unblocks them.

> Switch 2 is unverified — there are reports of tighter controller authorization than the
> original Switch. Worth testing before assuming the Pokken path carries over.

## Capture and inject

Because the VLA use case needs both directions, the two paths share one wire format: a
fixed-size POD struct mirroring `GamepadState`.

- **Inject** (policy → console): client writes the struct to a Unix `SOCK_SEQPACKET` socket.
  A `SocketSource` `recv()`s straight into the state — no parsing, and trivially drivable
  from Python via `struct.pack`.
- **Capture** (human → training set): the `Bridge` exposes an observer that records the same
  struct, stamped with **both** `CLOCK_MONOTONIC` (for interval math) and `CLOCK_REALTIME`
  (for aligning against camera frames). Records hand off to a logging thread through a
  preallocated ring so that logging never touches the hot path.

Using one struct for both means a captured session replays bit-exactly, which makes the
capture path its own regression test.

## Latency measurement

On-device loopback, no extra hardware: controller into a USB-A port, and the OTG cable's
USB-C data leg back into *another* USB-A port on the same Pi. `dwc2` (device) and RP1 (host)
are independent controllers, so the Pi can enumerate its own gadget.

This measures **evdev event arrival → loopback host reads the corresponding report**, i.e.
the entire span the software controls, including the gadget's polling interval. It cannot
measure the controller's own contribution, since the button press is not programmatic; for
that, bridge a GPIO across a button's contacts and timestamp GPIO-assert → evdev-arrival.

Primary experiment: whether the target host honors a `bInterval` of 1ms instead of the stock
descriptor's ~5ms.

## Building

No external dependencies -- the evdev source reads `struct input_event` off the character
device directly rather than linking libevdev, which is one fewer thing to cross-compile.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Produces `build/gpbridge` and `build/gpb-discover`.

## Bringing it up on the Pi

```bash
# 0. dependencies (optional -- build.sh falls back to plain g++ if cmake is absent)
sudo ./scripts/install_deps.sh
./scripts/build.sh

# 1. one-time: switch the USB-C port to peripheral mode, then reboot
sudo ./scripts/enable_gadget_mode.sh
sudo reboot

# 2. each boot: create the HID gadget
sudo ./scripts/gadget_up.sh
cat /sys/class/udc/*/current_speed        # want "high-speed" -- see note below

# 3. learn the controller's real mapping, and paste the result into the config
./build/gpb-discover list
sudo ./build/gpb-discover wizard /dev/input/by-id/usb-Google_LLC_Stadia_Controller...

# 4. run
sudo ./build/gpbridge --config config/stadia_to_switch.ini

# capture a session for training data
sudo ./build/gpbridge --config config/stadia_to_switch.ini --record session.bin

# replay it, or drive from a policy, over the socket
sudo ./build/gpbridge --config config/stadia_to_switch.ini --source socket
```

### If `gadget_up.sh` says "no UDC found"

A freshly flashed Raspberry Pi OS image (verified on Debian 13 / kernel 6.18, Pi 5) ships
`config.txt` already containing:

```
otg_mode=1
dtoverlay=dwc2,dr_mode=host
```

So `dwc2` is loaded, but bound as a **host**. `/sys/class/udc` is empty and the gadget has
nothing to bind to. `scripts/enable_gadget_mode.sh` rewrites that one word to `peripheral`
(keeping a timestamped backup); `--host` reverts it and `--show` just reports the current
state. A reboot is required either way.

### On the polling interval

The earlier plan was to tune the gadget's `bInterval` down from ~5ms to 1ms. That turns out
not to be a userspace knob: the `f_hid` driver hardcodes the endpoint interval and does not
expose it through configfs (the kernel source carries a long-standing FIXME saying as much).
At **high speed it already works out to 1ms**, which is the value we wanted anyway.

So the thing to verify is not the descriptor but the link speed. If `current_speed` reports
full-speed rather than high-speed, the interval is an order of magnitude worse, and that --
not anything in this codebase -- is the latency problem worth chasing.

## Project layout

```
include/gpb/          public headers, one per concept
  gamepad_state.hpp     the normalized POD struct; also the wire and capture format
  input_source.hpp      abstract source
  output_sink.hpp       abstract sink, with the separate initialize() phase
  transform.hpp         mapping/profile layer + the custom-injection hook
  bridge.hpp            composition root and event loop
src/sources/          EvdevSource (all physical pads), SocketSource (programmatic)
src/sinks/            NsHidSink (Switch, via the Pokken descriptor)
tools/discover.cpp    gpb-discover: list / caps / wizard
scripts/              gadget_up.sh, gadget_down.sh
config/               stadia_to_switch.ini
```

## What has been verified, and what has not

Tested on an x86 Linux box, substituting a regular file for `/dev/hidg0`:

- Config parsing, factory selection, the full socket -> transform -> encode -> write path.
- Report encoding: face buttons by physical position, hat encoding including opposite-pair
  cancellation (up+down together yields neutral rather than a nonsense direction), axis
  scaling, and the neutral report written at enumeration.
- Capture, and **bit-exact replay**: recording a session and feeding those records back
  through the socket produced byte-identical HID output. This is what the
  `emits_canonical()` flag protects -- see below.

Verified on the target hardware (Pi 5 Model B Rev 1.0, Bookworm, kernel 6.6.51, arm64):

- Builds clean on gcc 12/14 and aarch64 with `-Wall -Wextra -Wpedantic`, via both cmake and
  the bare-g++ fallback.
- `dwc2` peripheral mode, gadget creation, and **enumeration**: the host sees
  `0f0d:00c1 Hori Co., Ltd HORIPAD for Nintendo Switch` at **high speed**, and the HID
  descriptor parses into a working gamepad input device.
- **Full round trip through real USB.** Injecting states over the socket and reading them
  back off the host's own USB stack reproduces them exactly -- axes, hat, and every button
  bit landing where the descriptor says it should.
- **Measured latency: 0.8-1.0 ms** for inject -> gadget -> host USB -> evdev. That is the
  1 ms high-speed polling interval, confirming the interval dominates and that there is
  nothing left to win in userspace.
- Controller input: axis ranges read via `EVIOCGABS` (the Stadia's sticks report
  `min=1 max=255`), mapping measured with `gpb-discover wizard`, bit-exact capture/replay,
  hot-plug recovery, and both services surviving reboot.

Still unverified, because each needs a human or a console in the loop:Still unverified, because each needs a human or a console in the loop:

- **Whether the gadget ever enumerates.** Blocked on the Trixie/6.18 `dwc2` regression
  above, not on anything in this repo: the Pi has never seen a host attach, so nothing
  downstream of that has been exercised against real USB.
- Whether the Switch accepts the descriptor. The Pokken report layout is reproduced from
  prior art, not measured.
- **Whether a Switch 2 is a viable target at all.** Nintendo gates the Switch 2's USB-C
  port behind proprietary vendor-defined signalling, which broke third-party docks and was
  re-broken by a later firmware update. Whether that gating also covers ordinary USB HID
  devices on the dock's USB-A ports is unknown. The original Switch remains the validated
  target.

The controller mapping is no longer a guess: `config/stadia_to_switch.ini` holds what
`gpb-discover wizard` measured on real hardware, including the inverted face-button aliases
and the conventional `ABS_BRAKE`=left / `ABS_GAS`=right trigger assignment.

### The Stadia controller re-enumerates on its own

Not a fault, and not something to debug when you see it. `dmesg` shows the pad dropping off
the bus and coming straight back as a new device number, same serial, unprompted:

```
usb 3-1: USB disconnect, device number 3
usb 3-1: new high-speed USB device number 4 using xhci-hcd
```

The evdev node is destroyed and recreated, potentially as a different `eventN`. The bridge
treats this as a normal operating condition: it detects the loss, **releases the output to
neutral** (a pad that vanishes mid-press would otherwise leave the console seeing that
button held forever), and retries every 500ms, re-resolving the configured glob so a new
node number is picked up automatically. `--record` captures survive it; the stats line
reports disconnect and reconnect counts.

### Three traps worth knowing about

**evdev's face-button aliases lie about position -- confirmed on hardware.** `BTN_X` is an
alias for `BTN_NORTH` and `BTN_Y` for `BTN_WEST`, but on this layout X sits *west* and Y
sits *north*. Measured on a real Stadia controller, the wizard produced:

```
button.BTN_NORTH = west
button.BTN_WEST  = north
```

Reading the code names would have given the opposite and shipped X and Y swapped on the
Switch -- which would have presented as a mapping *preference* complaint rather than a bug,
and could have survived a long time. This is why the wizard asks you to press the *top*
button and records whatever code actually arrives.

**A dead fd spins epoll.** `epoll` keeps reporting `EPOLLERR`/`EPOLLHUP` on a descriptor
whose device has gone away, so a read loop that logs the error and returns will be re-entered
immediately, forever, emitting one line per iteration. A source that can vanish has to drop
the descriptor and say so, not just report the errno.

**Transforms are not idempotent.** Deadzone and expo rescale a stick, so applying them to
an already-transformed capture silently produces different values than the original run --
which would quietly invalidate the whole capture/replay guarantee. `InputSource::
emits_canonical()` marks sources that already speak post-transform action space
(`SocketSource` does), and the Bridge skips the mapping layer for them.

## Status

Working, pending a real console. The full chain is verified on hardware: controller in,
normalized, encoded, out over USB as a HORIPAD, and read back correctly by a host at
0.8-1.0 ms round trip. Both services come up on boot and recover from the controller's
self-re-enumeration.

Remaining: plug into an actual Nintendo Switch. Also open is whether a Switch 2 is a viable
target at all, given Nintendo's proprietary USB-C authentication.

Not yet done: the XInput/PC sink, cross-compilation.