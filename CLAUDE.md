# Notes for agents working on this repo

Start with [README.md](README.md) for what the project is and how it fits together. This file
covers the things that are not obvious from the code: how to get a change onto real hardware,
the conventions that are deliberate rather than accidental, and what is known to be unfinished.

## Hardware in the loop

There is one Raspberry Pi 5 used as the development target. **Ask the user for its address** —
it is deliberately not recorded here. SSH access is key-based and `sudo` is passwordless.

Almost nothing in this project can be verified without it. The build and the test suite run
anywhere, but USB gadget enumeration, controller input, and anything involving a console
require the Pi. Say so plainly rather than implying a change is verified when only its logic
is.

```bash
ssh <pi> 'cd ~/rpi_gamepad_bridge && git pull && ./scripts/build.sh \
          && sudo systemctl restart gpbridge gpb-web'
```

Deploys there are done with `git fetch && git reset --hard origin/<branch>`, so **anything
uncommitted on the Pi is destroyed**. The user keeps a local, untracked `config/my_network.ini`
for exactly this reason; do not suggest editing tracked config files on the Pi.

Web panel credentials live in `/etc/gpbridge/webpass` and `/etc/gpbridge/webuser`, readable
with `sudo`. Service state is `/var/lib/gpbridge/` — never `/etc`, which is root-owned so the
unprivileged panel cannot write there.

## Building and testing

```bash
./scripts/build.sh          # no dependencies; cmake if present, else plain g++
./tests/run_tests.sh        # the whole suite; needs nothing but python3 and a compiler
```

The suite runs with no Pi, no gadget and no controller: `SocketSource` stands in for a
controller and a regular file for `/dev/hidg0`. Some checks skip without `node` or
`/dev/uinput`; CI has both, so the full set runs there.

## Conventions that are deliberate

**Zero external dependencies, enforced.** `tests/check_stdlib_only.py` fails the build on any
import outside the standard library, and the C++ links nothing. This is why `build.sh` works on
a freshly flashed Pi before `apt` is touched, and why the client libraries add nothing to a
project that submodules them. Read that file's docstring before adding a dependency — "add a
venv" is usually not the right answer.

**Verify a test fails on the bug it targets.** This has caught two invalid tests here: one
drove the socket source, which by design skips transforms, so it passed with the bug present;
another grepped for a string the failure message never contained. A test that has only ever
passed is not evidence.

**A noisy check is worse than none.** A linter for dangling prose was written, fired twice on
correct text, and was deleted. Enforce invariants you can state precisely.

**Never report health you cannot verify.** `/sys/class/udc` once read `configured` while every
write failed — the panel showed a healthy link through a total outage, which is worse than
showing nothing. The panel now says *sent*, not *delivered*, for published capture, because
UDP cannot tell the sender whether anything arrived.

**One declaration of a fact.** A config declares its own `source`; nothing overrides it. An
earlier `GPB_SOURCE` override silently won over the config and cost an afternoon.

**Sinks accept either representation where they reasonably can.** Capabilities exist so a
client can send the *right* thing, not as grounds for rejecting the other one.

## Where the traps are already written down

Do not re-derive these; they are documented where someone would hit them:

| Trap | Where |
|---|---|
| Charge-only cables fail completely silently | README, under Parts |
| `heartbeat_hz = 0` makes sticks work and buttons not | README, Configuration |
| evdev's `BTN_NORTH`/`BTN_WEST` lie about position | both mapping inis in `config/`, measured on two pads |
| A d-pad may be a hat, not four buttons | `config/dualsense_to_switch.ini` |
| A held control is not a resting control | comments in `tools/discover.cpp` |
| CSS stacking and `[hidden]` overrides | assertions and comments in `tests/run_tests.sh` |

## Workflow

`main` is protected: PR required, CI required, linear history, no force pushes. Branch, open a
PR, squash merge. Admin bypass exists but should not be used casually.

Commit messages here explain *why*, including what was tried and rejected. Match that.

## Known unfinished

- **Rendered UI behaviour is untested.** Three CSS/DOM bugs shipped in one branch, all invisible
  to every static check. Specific invariants are now asserted, but that is a ratchet, not
  coverage. The user is effectively the render test; do not claim UI work is verified when only
  its API is.
- **PS4/PS5 and Xbox targets are blocked by licensed authentication silicon**, not by effort.
- **Cross-machine capture delivery is unverifiable from the Pi.** Only the receiver knows what
  arrived; correlating the two ends belongs in whatever repo submodules this one.
- Cross-compilation and an XInput/PC sink are scoped but not started.
