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
CONF_DIR=/etc/gpbridge
STATE_DIR=/var/lib/gpbridge
ENVFILE="$STATE_DIR/active.env"

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

# Which config and source the bridge starts with now lives in an env file rather than being
# baked into ExecStart, so the web panel can change it without rewriting a unit.
# Mutable state lives in /var/lib and is owned by the service account; /etc/gpbridge stays
# root-owned for secrets. The service must be able to CREATE files in this directory, not
# just write the env file: the update is done by writing a temp file alongside it and
# renaming, which is what makes it atomic.
mkdir -p "$CONF_DIR" "$STATE_DIR"
chown "${SUDO_USER:-${USER:-pi}}" "$STATE_DIR"

# Migrate an env file left in /etc by an earlier install.
if [[ -f "$CONF_DIR/active.env" && ! -f "$ENVFILE" ]]; then
  mv "$CONF_DIR/active.env" "$ENVFILE"
  chown "${SUDO_USER:-${USER:-pi}}" "$ENVFILE"
  echo "moved active.env to $STATE_DIR (service state does not belong in /etc)"
fi

if [[ ! -f "$ENVFILE" ]]; then
  cat > "$ENVFILE" <<ENV
# Written by gpb-web. The .ini files remain the source of truth;
# this only selects which one the service starts with.
GPB_CONFIG=$CONFIG
GPB_SOURCE=evdev
ENV
  echo "wrote $ENVFILE"
else
  echo "keeping existing $ENVFILE ($(grep -m1 '^GPB_CONFIG=' "$ENVFILE" | cut -d= -f2-))"
fi

for u in gpb-gadget gpbridge; do
  sed -e "s|@REPO@|$REPO|g" -e "s|@CONFIG@|$CONFIG|g" -e "s|@ENVFILE@|$ENVFILE|g" \
      -e "s|@STATEDIR@|$STATE_DIR|g" \
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
