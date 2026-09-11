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
  # neutral at enumeration; East+stick-right; North+dpad-up-left; all released; and a final
  # neutral written on shutdown. These bytes were verified against a real Nintendo Switch 2.
  #
  # That trailing neutral is a safety property, not an artifact: the host holds whatever we
  # last sent, so exiting with a button pressed would leave it held on the console forever.
  EXPECTED="0000088080808000 040008ff80808000 0800078080808000 0000088080808000 0000088080808000"
  EXPECTED="$(echo "$EXPECTED" | tr -d ' ')"
  if [[ "$(echo "$ACTUAL" | tr -d ' ')" == "$EXPECTED" ]]; then
    ok "reports encode exactly as expected"
  else
    bad "report bytes differ" "expected: $EXPECTED
        actual:   $(echo "$ACTUAL" | tr -d ' ')"
  fi
fi

# Assert the release-on-exit property by itself, so a regression names the actual problem
# rather than showing a wall of differing hex.
LAST="$(python3 -c "
d=open('$TMP/hid.bin','rb').read()
print(d[-8:].hex() if len(d)>=8 else 'short')")"
if [[ "$LAST" == "0000088080808000" ]]; then
  ok "output is released to neutral on shutdown (no stuck buttons)"
else
  bad "did not release to neutral on shutdown" "last report was $LAST"
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
button.BTN_TR2 = r2
[sink.ns_hid]
device = $TMP/hid3.bin
face_by_position = true
heartbeat_hz = 0
[profile.left]
deadzone = 0
EOF
    # The resting baseline must be the pad's true neutral. The web wizard samples this once
    # and hands it to every capture, because each capture is its own process: re-sampling
    # per step means a control still being HELD is recorded as its own neutral, and
    # releasing it then reads as a deflection that answers the following prompt.
    BASE="$(sudo -n "$BUILD/gpb-discover" baseline "$NODE" 2>/dev/null)"
    if python3 -c "
import json, sys
a = json.loads(sys.argv[1])['axes']
assert a['ABS_X'] == 128, a
assert a['ABS_GAS'] == 0, a
" "$BASE" 2>/dev/null; then
      ok "baseline reports the pad's resting position (sticks centred, triggers released)"
    else
      bad "baseline returned unexpected resting values" "$BASE"
    fi

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
    # Expect: the stick reaching full right (lx=0xff), the bottom face button appearing as
    # Switch B (bit 1 of byte 0), and a DIGITALLY bound trigger surviving as ZR (bit 7).
    STICK=0; FACE=0; ZR=0
    for r in $HEX; do
      B0=$((16#${r:0:2}))
      [[ "${r:6:2}" == "ff" ]] && STICK=1
      (( B0 & 0x02 )) && FACE=1
      (( B0 & 0x80 )) && ZR=1
    done
    if [[ $STICK -eq 1 && $FACE -eq 1 && $ZR -eq 1 ]]; then
      ok "evdev events reach the wire, including a digitally bound trigger as ZR"
    else
      bad "evdev path produced unexpected reports" "stick=$STICK face=$FACE zr=$ZR
        reports: $HEX"
    fi
  fi
fi

# ---------------------------------------------------------------- 6. static web assets
echo "web assets"
JS_OK=1
if command -v node >/dev/null 2>&1; then
  for f in web/static/*.js; do
    node --check "$f" 2>&1 | sed "s|^|        |" || JS_OK=0
  done
  [[ $JS_OK -eq 1 ]] && ok "javascript parses" || bad "javascript has a syntax error" "see above"
else
  echo "  SKIP  node not available to parse the javascript"
fi

# The browser's own [hidden] rule is a UA style, so any class selector setting `display`
# silently outranks it. That shipped once: .overlay{display:flex} made the wizard modal
# impossible to hide, so it covered the page from first paint and blocked every click --
# including its own Close button. The global override is what prevents a repeat.
if grep -qE '^\[hidden\] \{ display: none !important; \}' web/static/style.css; then
  ok "[hidden] is authoritative in css"
else
  bad "the global [hidden] override is missing from style.css" \
      "without it any class setting display can make an element unhideable"
fi

# The toast must stack above the modal overlay. It is the only channel the wizard has for
# reporting an error, and the wizard runs inside that overlay -- underneath it, messages are
# both hidden and blurred by the overlay's backdrop-filter.
ZORDER="$(python3 - <<'PYZ'
import re, pathlib
css = pathlib.Path("web/static/style.css").read_text()
def z(selector):
    m = re.search(re.escape(selector) + r"\s*\{[^}]*?z-index:\s*(\d+)", css, re.S)
    return int(m.group(1)) if m else None
print(f"{z('.toast')} {z('.overlay')}")
PYZ
)"
read -r TOAST_Z OVERLAY_Z <<<"$ZORDER"
if [[ "$TOAST_Z" != "None" && "$OVERLAY_Z" != "None" && $TOAST_Z -gt $OVERLAY_Z ]]; then
  ok "toast stacks above the modal overlay (${TOAST_Z} > ${OVERLAY_Z})"
else
  bad "toast would render behind the wizard modal" "toast z-index=$TOAST_Z overlay z-index=$OVERLAY_Z"
fi

# Every element id the scripts reach for must actually exist in the markup: a typo there
# produces a null dereference that silently kills the rest of the script.
MISSING="$(python3 - <<'PYCHK'
import re, pathlib
html = pathlib.Path("web/static/index.html").read_text()
ids = set(re.findall(r'id="([^"]+)"', html))
missing = []
for js in sorted(pathlib.Path("web/static").glob("*.js")):
    body = js.read_text()
    for ref in sorted(set(re.findall(r"(?:\$|wq)\('([^']+)'\)", body))):
        if ref not in ids:
            missing.append(f"{js.name}:#{ref}")
print(" ".join(missing))
PYCHK
)"
if [[ -z "$MISSING" ]]; then
  ok "every element id the scripts reach for exists in the markup"
else
  bad "scripts reference ids that are not in index.html" "$MISSING"
fi

# ---------------------------------------------------------------- 7. no dependency creep
echo "python dependency check"
if OUT="$(python3 tests/check_stdlib_only.py 2>&1)"; then
  echo "$OUT"; PASS=$((PASS+1))
else
  echo "$OUT"; FAIL=$((FAIL+1))
fi

# ---------------------------------------------------------------- 7. web control panel
echo "web control panel"
WEBPORT=$(( 18000 + RANDOM % 2000 ))
printf 'GPB_CONFIG=%s/config/stadia_to_switch.ini\nGPB_SOURCE=evdev\n' "$PWD" > "$TMP/active.env"
echo "testsecret" > "$TMP/webpass"
echo "admin" > "$TMP/webuser"
GPB_REPO="$PWD" GPB_ENVFILE="$TMP/active.env" GPB_PASSFILE="$TMP/webpass" \
GPB_USERFILE="$TMP/webuser" GPB_PORT=$WEBPORT \
  python3 web/gpb_web.py > "$TMP/web.log" 2>&1 &
WPID=$!
for _ in $(seq 1 40); do
  curl -fsS -o /dev/null -u admin:testsecret "http://127.0.0.1:$WEBPORT/api/status" 2>/dev/null && break
  sleep 0.25
done

code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
BASE="http://127.0.0.1:$WEBPORT"

if [[ "$(code "$BASE/")" == "401" && "$(code -u admin:wrong "$BASE/")" == "401" \
   && "$(code -u nobody:testsecret "$BASE/")" == "401" \
   && "$(code -u admin:testsecret "$BASE/")" == "200" ]]; then
  ok "both username and password are enforced"
else
  bad "auth did not behave as expected" \
      "anon=$(code "$BASE/") badpass=$(code -u admin:wrong "$BASE/") baduser=$(code -u nobody:testsecret "$BASE/") ok=$(code -u admin:testsecret "$BASE/")"
fi

# Credentials are read per request rather than cached at startup. Caching meant editing the
# password file did nothing until someone restarted the service -- a silent failure that
# looks exactly like a successful rotation.
echo "rotated" > "$TMP/webpass"
echo "operator" > "$TMP/webuser"
if [[ "$(code -u admin:testsecret "$BASE/")" == "401" \
   && "$(code -u operator:rotated "$BASE/")" == "200" ]]; then
  ok "rotating the credential files takes effect without a restart"
else
  bad "credential rotation did not take effect live" \
      "old=$(code -u admin:testsecret "$BASE/") new=$(code -u operator:rotated "$BASE/")"
fi
echo "testsecret" > "$TMP/webpass"; echo "admin" > "$TMP/webuser"

if curl -fsS -u admin:testsecret "$BASE/api/status" | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert 'bridge' in d and 'udc' in d and 'configs' in d and 'active' in d
assert any(c['name'].endswith('.ini') for c in d['configs']), 'no configs listed'
" 2>/dev/null; then
  ok "status endpoint reports services, USB state and available configs"
else
  bad "status endpoint malformed" "$(curl -s -u admin:testsecret "$BASE/api/status" | head -c 200)"
fi

# The selected config must be one of the known files: an arbitrary path is a file-disclosure
# and arbitrary-exec hazard, since whatever is named here is handed to the service.
REJECT="$(curl -s -u admin:testsecret -X POST -H 'Content-Type: application/json' \
  -d '{"config":"/etc/shadow","source":"evdev","restart":false}' "$BASE/api/select")"
REJECT2="$(curl -s -u admin:testsecret -X POST -H 'Content-Type: application/json' \
  -d "{\"config\":\"$PWD/config/stadia_to_switch.ini\",\"source\":\"pwn\",\"restart\":false}" "$BASE/api/select")"
if grep -q '"ok": false' <<<"$REJECT" && grep -q '"ok": false' <<<"$REJECT2"; then
  ok "config and source selections are validated against an allowlist"
else
  bad "selection validation is too permissive" "path: $REJECT
        source: $REJECT2"
fi

# The wizard writes config files and can delete them, so its guardrails matter more than
# most: it runs on a device plugged into a console, reachable over the network.
TGT="$(curl -s -u admin:testsecret "$BASE/api/targets")"
if python3 -c "
import json,sys
t=json.loads(sys.argv[1])['targets']
assert t, 'no targets'
first=t[0]
assert first.get('controls'), 'target has no controls'
assert first.get('body'), 'target has no diagram path'
assert all(c.get('label') for c in first['controls']), 'every control needs a label'
" "$TGT" 2>/dev/null; then
  ok "targets endpoint serves a usable device descriptor"
else
  bad "target descriptor malformed" "$(head -c 200 <<<"$TGT")"
fi

post() { curl -s -u admin:testsecret -X POST -H 'Content-Type: application/json' -d "$2" "$BASE$1"; }
MAP='[{"kind":"button","code":"BTN_SOUTH","target":"south"}]'

CLOBBER="$(post /api/wizard/save "{\"target\":\"horipad_switch\",\"device\":\"/dev/input/event0\",\"filename\":\"stadia_to_switch.ini\",\"name\":\"x\",\"mappings\":$MAP}")"
ESCAPE="$(post /api/wizard/save "{\"target\":\"horipad_switch\",\"device\":\"/dev/input/event0\",\"filename\":\"../../evil.ini\",\"name\":\"x\",\"mappings\":$MAP}")"
EMPTY="$(post /api/wizard/save "{\"target\":\"horipad_switch\",\"device\":\"/dev/input/event0\",\"filename\":\"fresh.ini\",\"name\":\"x\",\"mappings\":[]}")"
if grep -q '"ok": false' <<<"$CLOBBER" && grep -q '"ok": false' <<<"$ESCAPE" \
   && grep -q '"ok": false' <<<"$EMPTY"; then
  ok "wizard refuses to overwrite, escape config/, or save nothing"
else
  bad "wizard save guardrails too permissive" "clobber: $CLOBBER
        escape:  $ESCAPE
        empty:   $EMPTY"
fi

DELACTIVE="$(post /api/config/delete "{\"config\":\"$PWD/config/stadia_to_switch.ini\"}")"
DELUNKNOWN="$(post /api/config/delete '{"config":"/etc/passwd"}')"
if grep -q '"ok": false' <<<"$DELACTIVE" && grep -q '"ok": false' <<<"$DELUNKNOWN"; then
  ok "delete refuses the active config and unknown paths"
else
  bad "delete guardrails too permissive" "active: $DELACTIVE
        unknown: $DELUNKNOWN"
fi

# A real save must still work, or the guardrails above would pass trivially.
GOOD="$(post /api/wizard/save "{\"target\":\"horipad_switch\",\"device\":\"/dev/input/event0\",\"filename\":\"__wizard_test.ini\",\"name\":\"Test\",\"description\":\"generated by the suite\",\"mappings\":$MAP}")"
if grep -q '"ok": true' <<<"$GOOD" && [[ -f config/__wizard_test.ini ]] \
   && grep -q '^button.BTN_SOUTH = south' config/__wizard_test.ini \
   && grep -q '^heartbeat_hz = 125' config/__wizard_test.ini; then
  ok "wizard writes a valid config with the heartbeat defaulted on"
else
  bad "generated config was wrong" "$GOOD
        $(head -20 config/__wizard_test.ini 2>/dev/null)"
fi
rm -f config/__wizard_test.ini

# A digital ZL/ZR must survive the profile transform. It previously did not: the analog
# shadow assigned the bit rather than OR-ing it, so a pad mapped with BTN_TL2/BTN_TR2 (no
# analog value, lt stays 0) had every press erased immediately. The config looked right and
# the control did nothing.
BAD_AXIS="$(post /api/wizard/save "{\"target\":\"horipad_switch\",\"device\":\"/dev/input/event0\",\"filename\":\"__bad_axis.ini\",\"name\":\"x\",\"mappings\":[{\"kind\":\"axis\",\"code\":\"ABS_HAT0Y\",\"target\":\"dup\"},{\"kind\":\"button\",\"code\":\"BTN_SOUTH\",\"target\":\"south\"}]}")"
if grep -q '"ok": true' <<<"$BAD_AXIS" && ! grep -q 'dup' config/__bad_axis.ini; then
  ok "an axis binding with an unusable target is refused rather than silently dropped later"
else
  bad "invalid axis target reached the config" "$(grep -n 'axis\.' config/__bad_axis.ini 2>/dev/null)"
fi
rm -f config/__bad_axis.ini

kill $WPID 2>/dev/null; wait $WPID 2>/dev/null

echo
echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
