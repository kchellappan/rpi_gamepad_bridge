# rpi_gamepad_bridge

> ### 🤖 Heavily AI-generated
>
> Almost all of the code, scripts, tests and documentation in this repository were written
> by **Claude (Anthropic)**, working from a human-specified architecture and driven through
> hardware bring-up interactively. It is not a toy: the result has been validated on real
> hardware, including playing on a Nintendo Switch 2. But it has had no independent human
> code review, so read it before you trust it with anything that matters.

A low-latency bridge that turns a Raspberry Pi 5 into a USB gamepad. It reads from some
input peripheral — a wired controller, or an application driving it programmatically — and
re-emits that input as a USB HID gamepad that a console accepts as a real controller.

Measured round-trip latency is **0.8–1.0 ms**, which is the USB polling interval; there is
essentially no software overhead on top of the wire.

## Why it exists

Because the bridge sits between an input and a host, anything that can produce gamepad state
can drive a console that would otherwise only accept a first-party controller:

- **Bridging non-traditional control schemes.** Connect an input the console has never heard
  of — a CAN joystick, custom or adaptive hardware, an accessibility device — to a host that
  only speaks standard USB HID. The input does not have to be a gamepad, or even reach the
  kernel through evdev: a source is anything that can produce state and expose a pollable
  descriptor, so a fieldbus, a serial protocol or a network feed all qualify. Adding one is
  a new `InputSource`, not a rewrite.
- **Automation.** Drive a console programmatically over a socket: step through a fixed
  sequence, script a repetitive task, or exercise something the same way many times over.
- **Imitation learning.** Capture human gameplay as training data for a VLA
  (vision-language-action) model, then drive the same console from that model's inferred
  actions. Capture and replay are two directions of one pipeline and are bit-for-bit
  identical, so a recorded session replays exactly.

---

## Parts

| Part | Notes |
|---|---|
| Raspberry Pi 5 | The USB-C port is the only OTG-capable one, and it is USB 2.0 data only |
| [USB-C power/data splitter](https://www.amazon.com/Charging-Adapter-Splitter-Compatible-Chromecast/dp/B0B5MPCJF5/) | The Pi's single USB-C port is both power in and data out; this separates them |
| A USB-A-to-C cable for the data leg | |
| A wired controller | Anything the kernel exposes through evdev |
| The target console | Connect the data leg to a USB-A port on its dock |

> ⚠️ **Use cables with data lines.** Charge-only USB cables are common and look identical to
> data cables. With one in the data leg, nothing works and *nothing reports an error* — the
> gadget never sees a host, the host never sees a device, and every layer looks healthy. If
> the Pi never leaves `state = not attached`, suspect the cable first.

## Quick start

```bash
git clone https://github.com/kchellappan/rpi_gamepad_bridge.git
cd rpi_gamepad_bridge

./scripts/build.sh                  # no dependencies; uses cmake if present, else g++

sudo ./scripts/enable_gadget_mode.sh   # puts the USB-C port in peripheral mode
sudo reboot

sudo ./scripts/install_services.sh  # gadget + bridge, started at boot
sudo ./scripts/install_web.sh --user you   # control panel on :8080
```

The install script prints a URL and a generated password. From there you can do most things
without SSH — see [Web control panel](#web-control-panel).

Then map your controller. The easiest way is **Start wizard** in the control panel, which
shows you the device being emulated and asks which of your controls should act as each of
its buttons.

There is a terminal equivalent if you prefer:

```bash
sudo systemctl stop gpbridge        # it holds the controller exclusively
./build/gpb-discover list
sudo ./build/gpb-discover wizard /dev/input/by-id/<your-controller>
sudo systemctl start gpbridge
```

Either way it records what actually arrives when you press something, which is the only
reliable way to get this right — evdev's button names do not always correspond to physical
positions.

## Running it by hand

```bash
sudo ./build/gpbridge --config config/stadia_to_switch.ini

# capture a session for training data
sudo ./build/gpbridge --config config/stadia_to_switch.ini --record session.bin

# drive it from an application, or replay a capture, over a Unix socket
sudo ./build/gpbridge --config config/stadia_to_switch.ini --source socket
```

## Web control panel

`http://<host>.local:8080/` — start, stop and restart the bridge, pick a config, switch
between controller and socket mode, and read the log, without SSH.

It runs as its own service (`gpb-web`), deliberately separate from `gpbridge`: the bridge is
a real-time loop and has no business hosting an HTTP server, and a process cannot cleanly
restart itself. It also stays up while the bridge is stopped, which is half of what it is for.

- **Auth** is HTTP basic. The username lives in `/etc/gpbridge/webuser` (default `admin`)
  and the password in `/etc/gpbridge/webpass`, generated at install. Both are read on every
  request, so changing them takes effect immediately:

  ```bash
  sudo ./scripts/install_web.sh --set-password        # or --set-password mysecret
  sudo ./scripts/install_web.sh --set-user karthik
  sudo ./scripts/install_web.sh --show                # print the URL and current username
  ```
- **Privileges** come from a narrow sudoers rule covering four specific `systemctl` calls and
  reading the bridge's journal — the server itself runs unprivileged.
- **Selection** is written to `/etc/gpbridge/active.env`, which the `gpbridge` unit reads.
  The `.ini` files stay the source of truth, so the panel and SSH never disagree.
- **No venv, no pip, no build step.** The server imports nothing outside the Python standard
  library, and CI enforces that rather than trusting it.

Changing a config restarts the bridge but leaves the USB device in place, so the console sees
a brief gap in reports rather than a controller disconnect. The **Re-enumerate gadget** button
exists for when something has genuinely wedged; it is never triggered automatically, because
re-enumeration *does* show the console a disconnect.

### Mapping wizard

The panel's wizard asks the question the useful way round. Rather than "press the bottom face
button" and inferring what you meant, it shows a diagram of the device being emulated,
highlights one control at a time, and asks which of *your* controls should act as it. You
state the intent directly, so nothing has to be inferred — and evdev's misleading
`BTN_NORTH`/`BTN_WEST` aliases stop mattering, because the code that arrives is simply
recorded against the control you were pointing at.

Each step times out and can be skipped, so a pad missing a control does not strand the run.
The result is written as a **new** config — it never overwrites an existing one — with the
name and description you give it. Configs can be deleted from the main page, except the one
currently selected.

Devices to emulate are data, not code: `targets/*.json` holds the control list, the labels,
and the diagram geometry. Adding another is a JSON file, the same way adding an input is a
new `InputSource`.

Capture runs in `gpb-discover`, not in the web server, so the wizard and the terminal share
one implementation of the parts that were hard to get right — resting baselines, the
observed-axis rule, and rejecting an `absinfo` value that falls outside the axis's own range.

### Latency measurement

The panel can measure the real input-to-output time. It needs a **loopback**: move the OTG
cable's data leg from the console into one of the Pi's own USB-A ports. `dwc2` (device) and
RP1 (host) are independent controllers, so the Pi enumerates its own gadget and can watch
what it sends.

The page walks through the setup, notices the loopback appearing, and then measures
continuously until you stop it or a minute elapses — whichever comes first. Samples,
median and worst case update live, and stopping early keeps everything collected so far.

The cap exists because the bridge is stopped for the duration: a forgotten tab should not
hold it down indefinitely.

What it measures:

```
/dev/hidg0 -> USB -> host -> evdev
```

The bridge is stopped for the run, because it holds the gadget open, and the report is
written directly. Its own processing — a read, an encode, a write — is microseconds against a
total set by the polling interval, so including it would change nothing this can resolve.

The controller's own latency is not included and cannot be, because nothing in software can
press a physical button — that needs a GPIO bridged across a button's contacts. The results
say so rather than quietly presenting a flattering number.

Expect roughly **1 ms**. On the [reference setup](#verified-configuration) — Raspberry Pi 5,
Bookworm with kernel 6.6.51, gadget enumerated at high speed — the median is **0.94 ms**, and
essentially every sample lands in a single 0.1 ms bin of the histogram. That spike is the
finding: the USB polling interval quantises everything, and no software overhead is visible
above it.

Buttons and axes are measured separately, alternating, because they ought to be identical —
every control shares one 8-byte report, so they ride the same interrupt transfer. On the same
setup: **buttons 0.941 ms, axes 0.939 ms**, 154 samples each. Worth checking rather than
assuming, because evdev applies fuzz filtering to absolute axes that has no button
equivalent; axis samples swing full scale so nothing can be filtered out from under them.

While the cable is looped back, the panel will report **"Looped back to this Pi"** rather
than a healthy link. That is correct: `usbhid` only polls a HID device's interrupt endpoint
while something has its input node open, so with no reader the reports simply queue. The
measurement opens the node itself, which is why it works regardless.

## Configuration

One INI file selects the source and sink, maps the controller, and shapes the sticks. Two
fully commented examples ship, both measured on real hardware:

- [`config/stadia_to_switch.ini`](config/stadia_to_switch.ini) — Google Stadia controller
- [`config/dualsense_to_switch.ini`](config/dualsense_to_switch.ini) — Sony DualSense (PS5)

They are worth reading side by side. The two pads disagree about which axis carries the
right stick, whether the d-pad is a hat or four buttons, and — for the same two evdev codes
— which physical positions they describe. That disagreement is the case for measuring a
controller rather than assuming it.

| Key | Meaning |
|---|---|
| `meta.name` / `meta.description` | Human-readable label shown in the control panel; falls back to the filename |
| `bridge.source` / `bridge.sink` | Which implementations to use (`evdev`, `socket` / `ns_hid`) |
| `bridge.record_path` | Capture destination; empty disables recording |
| `bridge.rt_priority`, `bridge.cpu_affinity` | Optional `SCHED_FIFO` priority and core pinning |
| `source.evdev.device` | Device path; globs are resolved at startup |
| `source.evdev.axis.*` / `.button.*` | Mapping, as produced by the wizard |
| `sink.ns_hid.face_by_position` | Keep the physical button position, or the printed letter |
| `sink.ns_hid.heartbeat_hz` | How often to resend state. **Do not set this to 0 for a console** — see below |
| `profile.left` / `profile.right` | Radial deadzone, saturation, expo curve, axis inversion |

### Heartbeat

Consoles expect a gamepad to report its complete state every polling interval, not just when
something changes. With `heartbeat_hz = 0` a button press exists in exactly one report out of
thousands of polls and is usually missed, while analog sticks appear to work perfectly
because their values are absolute. **Sticks working while buttons misbehave means the
heartbeat is off.** The default of 125 Hz matches a conventional USB gamepad.

## Programmatic control

`--source socket` accepts a fixed 48-byte `GamepadState` struct over a Unix `SOCK_SEQPACKET`
socket — the same struct `--record` writes, so captures replay byte-for-byte. From Python:

```python
import socket, struct
FMT = "<IHHIIQQhhhhBB6s"                       # 48 bytes
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("/run/gpbridge.sock")
EAST = 1 << 1
s.send(struct.pack(FMT, 0x52474231, 1, 48, 0, EAST, 0, 0, 32767, 0, 0, 0, 0, 0, b"\0" * 6))
```

Every record carries both `CLOCK_MONOTONIC` and `CLOCK_REALTIME` timestamps — the first for
interval math, the second for aligning a capture against an external recording such as video. See [`tests/drive.py`](tests/drive.py) for a working
client.

## Architecture

Three layers, joined by a normalized state struct so sources and sinks never know about each
other:

```
InputSource (abstract)          GamepadState              OutputSink (abstract)
  ├─ EvdevSource          ──►   (normalized,        ──►     ├─ NsHidSink   (Switch)
  ├─ SocketSource                POD, no alloc)             └─ ...
  └─ ...
                    ▲                                  ▲
                    └────────── Bridge ────────────────┘
```

- **`InputSource`** produces normalized state from a pollable fd. `EvdevSource` covers every
  physical controller the kernel already normalizes, which is most of them — so the
  abstraction is not there to abstract over gamepads. It earns its keep on inputs evdev
  *cannot* express: the socket, a CAN bus, a serial link, a raw HID device deliberately
  unbound from its driver.
- **`OutputSink`** serializes state into its target's wire format. `initialize()` is separate
  from the hot path, leaving room for a sink that must complete an authentication handshake.
- **`Transform`** is the mapping layer — deadzones, curves, remapping — and the hook for
  custom injection logic.
- **`Bridge`** owns one of each and runs a single-threaded `epoll` loop. It is a state
  holder, not a pipe: input arrives on change, while the sink emits on a fixed cadence.

Adding a source or sink is one file plus a registration in `src/factory.cpp`.

Reports are coalesced, never queued: if the host has not drained the previous report, the
newest state overwrites it. A queue would turn bounded latency into unbounded latency.

## Output targets

| Target | Supported |
|---|---|
| Nintendo Switch 2 | **Yes**, verified — via a USB-A port on the official dock |
| Nintendo Switch | **Yes** — same HID descriptor, not re-verified here |
| PC (Linux / DirectInput) | Possible; not implemented |
| PC (Windows / XInput) | Possible; requires impersonating an Xbox 360 pad. Not implemented |
| PS4 / PS5 | **Blocked.** The console requires a response signed by a licensed auth chip |
| Xbox One / Series | **Blocked.** The GIP protocol requires a licensed chip |

The PlayStation and Xbox blocks are cryptographic, not architectural — no amount of work
here unlocks them without the licensed silicon.

## Tools

| | |
|---|---|
| `gpbridge` | The bridge itself |
| `gpb-discover` | `list` devices, dump `caps`, or run the mapping `wizard` |
| `gpb-fakepad` | A uinput-backed virtual gamepad, for testing without hardware |
| `gpb-latency` | Loopback latency measurement; the panel drives it |
| `web/gpb_web.py` | The control panel; standard library only, no pip |

## Tests

```bash
./tests/run_tests.sh
```

Runs with no Pi, no gadget and no controller: the socket source stands in for a controller
and a regular file stands in for `/dev/hidg0`. Covers report encoding, release-on-shutdown,
capture/replay equivalence, wire-format stability, startup validation, the evdev path via a
virtual pad, control-panel auth and credential rotation, and a check that no Python file
imports outside the standard library. CI runs all of it on every push.

## Verified configuration

| | |
|---|---|
| Board | Raspberry Pi 5 Model B Rev 1.0 |
| OS | Raspberry Pi OS Bookworm, `2024-11-19` arm64, kernel `6.6.51` |
| Controllers | Google Stadia Controller rev. A, and Sony DualSense (PS5) — both wired |
| Console | Nintendo Switch 2, via the official dock |

Both controllers pass the console's own controller test on sticks and buttons. The DualSense
was mapped entirely through the web wizard with no code changes for the pad itself, which is
the `EvdevSource` abstraction doing what it is for.

Its layout differs from the Stadia's in ways worth knowing, because those differences are
what surfaced several bugs: its d-pad arrives as a **hat** rather than four buttons, its
triggers bind **digitally** as `BTN_TL2`/`BTN_TR2` rather than as analog axes, and it has no
control to spare for Capture — its Create button reports `BTN_SELECT`, which is already
Minus.

Newer kernels are untested, and every further controller is an untested case until someone
runs the wizard on it.

## License

[MIT](LICENSE).

*Descriptor and gadget setup derive from prior art in
[gdsports/NSGadget_Pi](https://github.com/gdsports/NSGadget_Pi), which is also MIT licensed.*
