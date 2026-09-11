#!/bin/bash
# Install systemd units so the gadget and the bridge survive a reboot.
#
# The USB-C port's peripheral mode persists (it lives in config.txt), but the gadget
# itself does not: configfs is rebuilt from scratch every boot, so /dev/hidg0 disappears
# and the bridge has nothing to open. That is what these units fix.
#
# Units point at this checkout rather than copying binaries into /usr/local, so the
# development loop stays short:
#
#     git pull && ./scripts/build.sh && sudo systemctl restart gpbridge
#
#   install_services.sh [config_path]     install and enable
#   install_services.sh --uninstall       stop, disable, remove
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR=/etc/systemd/system

if [[ "${1:-}" == "--uninstall" ]]; then
  [[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
  systemctl disable --now gpbridge.service gpb-gadget.service 2>/dev/null || true
  rm -f "$UNIT_DIR/gpbridge.service" "$UNIT_DIR/gpb-gadget.service"
  systemctl daemon-reload
  echo "removed."
  exit 0
fi

CONFIG="${1:-$REPO/config/stadia_to_switch.ini}"
[[ $EUID -eq 0 ]] || { echo "must run as root (sudo $0 $*)" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "no such config: $CONFIG" >&2; exit 1; }
[[ -x "$REPO/build/gpbridge" ]] || { echo "build first: $REPO/scripts/build.sh" >&2; exit 1; }

for u in gpb-gadget gpbridge; do
  sed -e "s|@REPO@|$REPO|g" -e "s|@CONFIG@|$CONFIG|g" \
      "$REPO/systemd/$u.service.in" > "$UNIT_DIR/$u.service"
  echo "wrote $UNIT_DIR/$u.service"
done

systemctl daemon-reload
systemctl enable gpb-gadget.service gpbridge.service
systemctl restart gpb-gadget.service
systemctl restart gpbridge.service

echo
systemctl --no-pager --lines=0 status gpb-gadget.service | head -4 || true
echo
systemctl --no-pager --lines=0 status gpbridge.service | head -4 || true
cat <<'NOTE'

  journalctl -u gpbridge -f      follow the bridge
  systemctl restart gpbridge     after a rebuild
  systemctl stop gpbridge        to run it by hand instead
NOTE
