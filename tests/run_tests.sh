#!/bin/bash
# Hardware-free test suite.
#
# Everything below runs on a stock CI runner with no Pi, no gadget and no controller. That
# is possible because of two design choices: SocketSource can stand in for a controller, and
# a regular file can stand in for /dev/hidg0, so the entire encode-and-write path is
# exercisable without USB.
#
# What is NOT covered, and cannot be: gadget enumeration, and how a console reacts.
set -uo pipefail

cd "$(dirname "$0")/.."
BUILD="${BUILD_DIR:-build}"
BRIDGE="$BUILD/gpbridge"
TMP="$(mktemp -d)"
SOCK="/tmp/gpbtest.$$.sock"   # short path: AF_UNIX sun_path is capped at ~108 bytes
trap 'rm -rf "$TMP" "$SOCK"' EXIT

PASS=0; FAIL=0
ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; echo "        $2"; FAIL=$((FAIL+1)); }

[[ -x "$BRIDGE" ]] || { echo "no $BRIDGE -- run scripts/build.sh first" >&2; exit 1; }

make_config() {
  cat > "$TMP/test.ini" <<EOF
[bridge]
source = socket
sink = ns_hid
${2:-}
[source.socket]
path = $SOCK
[sink.ns_hid]
device = $1
face_by_position = true
heartbeat_hz = 0
EOF
}

# ---------------------------------------------------------------- 1. report encoding
echo "encoding: socket -> transform -> encode -> write"
: > "$TMP/hid.bin"
make_config "$TMP/hid.bin"
rm -f "$SOCK"
"$BRIDGE" --config "$TMP/test.ini" > "$TMP/bridge.log" 2>&1 &
BPID=$!
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done

python3 tests/drive.py "$SOCK" encode > "$TMP/drive.log" 2>&1
DRIVE_RC=$?
sleep 0.4; kill -INT $BPID 2>/dev/null; wait $BPID 2>/dev/null

if [[ $DRIVE_RC -ne 0 ]]; then
  bad "client could not drive the bridge" "$(tail -3 "$TMP/drive.log")"
else
  ACTUAL="$(python3 -c "
import sys
d=open('$TMP/hid.bin','rb').read()
print(' '.join(d[i:i+8].hex() for i in range(0,len(d)//8*8,8)))")"
  # neutral at startup, then each injected state. Verified against real hardware.
  # neutral at enumeration, then: East+stick-right, North+dpad-up-left, all released.
  # These bytes were verified against a real Nintendo Switch 2.
  EXPECTED="0000088080808000 040008ff80808000 0800078080808000 0000088080808000"
  EXPECTED="$(echo "$EXPECTED" | tr -d ' ')"
  if [[ "$(echo "$ACTUAL" | tr -d ' ')" == "$EXPECTED" ]]; then
    ok "reports encode exactly as expected"
  else
    bad "report bytes differ" "expected: $EXPECTED
        actual:   $(echo "$ACTUAL" | tr -d ' ')"
  fi
fi

# ---------------------------------------------------------------- 2. capture/replay
echo "capture and replay are bit-exact"
: > "$TMP/hid1.bin"
make_config "$TMP/hid1.bin" "record_path = $TMP/capture.bin"
rm -f "$SOCK"
"$BRIDGE" --config "$TMP/test.ini" > "$TMP/b1.log" 2>&1 &
BPID=$!
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
python3 tests/drive.py "$SOCK" encode >/dev/null 2>&1
sleep 0.4; kill -INT $BPID 2>/dev/null; wait $BPID 2>/dev/null

: > "$TMP/hid2.bin"
make_config "$TMP/hid2.bin"
rm -f "$SOCK"
"$BRIDGE" --config "$TMP/test.ini" > "$TMP/b2.log" 2>&1 &
BPID=$!
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
python3 tests/drive.py "$SOCK" replay "$TMP/capture.bin" >/dev/null 2>&1
sleep 0.4; kill -INT $BPID 2>/dev/null; wait $BPID 2>/dev/null

if [[ ! -s "$TMP/capture.bin" ]]; then
  bad "capture file is empty" "recording produced nothing"
elif cmp -s "$TMP/hid1.bin" "$TMP/hid2.bin"; then
  ok "replaying a capture reproduces identical HID output ($(stat -c%s "$TMP/hid1.bin") bytes)"
else
  bad "replay diverged from the original" "$(cmp -l "$TMP/hid1.bin" "$TMP/hid2.bin" | head -3)"
fi

# ---------------------------------------------------------------- 3. wire format
echo "wire format"
SZ="$(python3 -c "import struct; print(struct.calcsize('<IHHIIQQhhhhBB6s'))")"
if [[ "$SZ" == "48" ]]; then
  ok "GamepadState is 48 bytes as the C++ static_assert requires"
else
  bad "wire struct size drifted" "python says $SZ, C++ asserts 48"
fi

# ---------------------------------------------------------------- 4. bad sink is rejected
echo "a sink pointed at an unusable device is refused at startup"
# A directory cannot be opened for writing, so it is a clean stand-in for a bad device path.
make_config "$TMP"
rm -f "$SOCK"
OUT="$(timeout 5 "$BRIDGE" --config "$TMP/test.ini" 2>&1)" || true
if grep -qi "cannot open HID gadget" <<<"$OUT"; then
  ok "unopenable sink device fails fast with a clear message"
else
  bad "expected a startup failure for an unopenable sink" "got: $(head -2 <<<"$OUT")"
fi

# ---------------------------------------------------------------- 5. evdev path (optional)
echo "evdev source end to end (needs uinput)"
FAKE="$BUILD/gpb-fakepad"
if [[ ! -x "$FAKE" ]]; then
  echo "  SKIP  gpb-fakepad not built"
elif ! sudo -n true 2>/dev/null; then
  echo "  SKIP  no passwordless sudo for /dev/uinput"
elif ! sudo -n modprobe uinput 2>/dev/null && [[ ! -e /dev/uinput ]]; then
  echo "  SKIP  /dev/uinput unavailable in this environment"
else
  : > "$TMP/hid3.bin"
  sudo -n "$FAKE" --emit-test > "$TMP/pad.log" 2>&1 &
  PADPID=$!
  sleep 1
  NODE="$(grep -o '/dev/input/event[0-9]*' "$TMP/pad.log" | head -1)"
  if [[ -z "$NODE" ]]; then
    echo "  SKIP  virtual pad did not appear ($(head -1 "$TMP/pad.log"))"
    wait $PADPID 2>/dev/null
  else
    cat > "$TMP/ev.ini" <<EOF
[bridge]
source = evdev
sink = ns_hid
[source.evdev]
device = $NODE
grab = false
axis.ABS_X = lx
axis.ABS_Y = ly
button.BTN_SOUTH = south
[sink.ns_hid]
device = $TMP/hid3.bin
face_by_position = true
heartbeat_hz = 0
[profile.left]
deadzone = 0
EOF
    sudo -n "$BRIDGE" --config "$TMP/ev.ini" > "$TMP/b3.log" 2>&1 &
    BPID=$!
    wait $PADPID 2>/dev/null
    sleep 0.4
    sudo -n kill -INT $BPID 2>/dev/null; wait $BPID 2>/dev/null
    HEX="$(python3 -c "
d=open('$TMP/hid3.bin','rb').read()
print(' '.join(d[i:i+8].hex() for i in range(0,len(d)//8*8,8)))")"
    # Expect the stick to reach full right (lx=0xff) and the bottom face button to appear
    # as Switch B (bit 1 of byte 0) under face_by_position.
    if grep -q "ff" <<<"$HEX" && grep -qE "02[0-9a-f]{2}" <<<"$HEX"; then
      ok "evdev events reach the wire correctly"
    else
      bad "evdev path produced unexpected reports" "reports: $HEX"
    fi
  fi
fi

echo
echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
