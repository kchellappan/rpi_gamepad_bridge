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

Then teach it your controller's layout and point the config at the result:

```bash
sudo systemctl stop gpbridge        # it holds the controller exclusively
./build/gpb-discover list
sudo ./build/gpb-discover wizard /dev/input/by-id/<your-controller>
sudo systemctl start gpbridge
```

The wizard prints a config block to paste into `config/`. It asks you to press each control
and records what actually arrives, which is the only reliable way to get this right —
evdev's button names do not always correspond to physical positions.

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

Changing a config restarts the bridge but leaves the USB device in place, so the console sees
a brief gap in reports rather than a controller disconnect. The **Re-enumerate gadget** button
exists for when something has genuinely wedged; it is never triggered automatically, because
re-enumeration *does* show the console a disconnect.

## Configuration

One INI file selects the source and sink, maps the controller, and shapes the sticks. See
[`config/stadia_to_switch.ini`](config/stadia_to_switch.ini) for a fully commented example.

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
| `web/gpb_web.py` | The control panel; standard library only, no pip |

## Tests

```bash
./tests/run_tests.sh
```

Runs with no Pi, no gadget and no controller: the socket source stands in for a controller
and a regular file stands in for `/dev/hidg0`. Covers report encoding, capture/replay
equivalence, wire-format stability, startup validation, and the evdev path via a virtual pad.
CI runs this on every push.

## Verified configuration

Verified against exactly one combination:

| | |
|---|---|
| Board | Raspberry Pi 5 Model B Rev 1.0 |
| OS | Raspberry Pi OS Bookworm, `2024-11-19` arm64, kernel `6.6.51` |
| Controller | Google Stadia Controller rev. A, wired |
| Console | Nintendo Switch 2, via the official dock |

Newer kernels are untested. Other controllers should work through the same `EvdevSource`
after a wizard run, but only the Stadia has been exercised.

## License

[MIT](LICENSE).

*Descriptor and gadget setup derive from prior art in
[gdsports/NSGadget_Pi](https://github.com/gdsports/NSGadget_Pi), which is also MIT licensed.*
