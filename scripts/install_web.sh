#!/bin/bash
# Install the web control panel.
#
# Grants the web user exactly three privileges through sudoers -- restarting the two units
# and reading the bridge's journal -- rather than running the server as root. The panel can
# stop a service and swap configs on a device plugged into a console, so it is worth keeping
# its reach narrow and legible.
#
#   install_web.sh [port]        install, enable and start (default port 8080)
#   install_web.sh --uninstall   remove the service, sudoers rule and password
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR=/etc/systemd/system
CONF_DIR=/etc/gpbridge
ENVFILE="$CONF_DIR/active.env"
PASSFILE="$CONF_DIR/webpass"
SUDOERS=/etc/sudoers.d/020_gpb-web
WEB_USER="${SUDO_USER:-${USER:-pi}}"

if [[ "${1:-}" == "--uninstall" ]]; then
  [[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 1; }
  systemctl disable --now gpb-web.service 2>/dev/null || true
  rm -f "$UNIT_DIR/gpb-web.service" "$SUDOERS" "$PASSFILE"
  systemctl daemon-reload
  echo "removed. gpbridge and gpb-gadget are untouched."
  exit 0
fi

PORT="${1:-8080}"
[[ $EUID -eq 0 ]] || { echo "must run as root (sudo $0 $*)" >&2; exit 1; }
[[ -x "$REPO/build/gpbridge" ]] || { echo "build first: $REPO/scripts/build.sh" >&2; exit 1; }

mkdir -p "$CONF_DIR"

# Seed the active selection if it does not exist yet, so the bridge has something to start
# with before anyone opens the page.
if [[ ! -f "$ENVFILE" ]]; then
  DEFAULT_CONFIG="$(ls -1 "$REPO"/config/*.ini 2>/dev/null | head -1 || true)"
  [[ -n "$DEFAULT_CONFIG" ]] || { echo "no config/*.ini to default to" >&2; exit 1; }
  cat > "$ENVFILE" <<ENV
# Written by gpb-web. The .ini files remain the source of truth;
# this only selects which one the service starts with.
GPB_CONFIG=$DEFAULT_CONFIG
GPB_SOURCE=evdev
ENV
  echo "==> seeded $ENVFILE -> $(basename "$DEFAULT_CONFIG")"
fi
chown "$WEB_USER" "$ENVFILE"

# Generate a password once. Regenerating on every install would silently invalidate a
# bookmark's saved credentials.
if [[ ! -f "$PASSFILE" ]]; then
  python3 -c "import secrets; print(secrets.token_urlsafe(18))" > "$PASSFILE"
  echo "==> generated a web password"
fi
chmod 640 "$PASSFILE"; chown root:"$WEB_USER" "$PASSFILE"

# Narrow sudoers rule: these exact commands, nothing else, no password prompt.
cat > "$SUDOERS" <<SUDO
# Installed by rpi_gamepad_bridge/scripts/install_web.sh
# The web control panel runs unprivileged and needs exactly these.
$WEB_USER ALL=(root) NOPASSWD: /usr/bin/systemctl start gpbridge.service
$WEB_USER ALL=(root) NOPASSWD: /usr/bin/systemctl stop gpbridge.service
$WEB_USER ALL=(root) NOPASSWD: /usr/bin/systemctl restart gpbridge.service
$WEB_USER ALL=(root) NOPASSWD: /usr/bin/systemctl restart gpb-gadget.service
$WEB_USER ALL=(root) NOPASSWD: /usr/bin/journalctl -u gpbridge.service *
SUDO
chmod 440 "$SUDOERS"
# A malformed sudoers file can lock the machine out of sudo entirely, so validate and
# remove it rather than leave it in place.
if ! visudo -cf "$SUDOERS" >/dev/null; then
  rm -f "$SUDOERS"
  echo "generated sudoers rule failed validation; removed it" >&2
  exit 1
fi
echo "==> sudoers rule installed and validated ($SUDOERS)"

sed -e "s|@REPO@|$REPO|g" -e "s|@USER@|$WEB_USER|g" \
    -e "s|@ENVFILE@|$ENVFILE|g" -e "s|@PORT@|$PORT|g" \
    "$REPO/systemd/gpb-web.service.in" > "$UNIT_DIR/gpb-web.service"

systemctl daemon-reload
systemctl enable gpb-web.service >/dev/null
systemctl restart gpb-web.service

IP="$(hostname -I | awk '{print $1}')"
cat <<DONE

  Control panel:  http://$(hostname).local:$PORT/
                  http://$IP:$PORT/

  Username:       anything
  Password:       $(cat "$PASSFILE")

  Run scripts/install_services.sh as well if you have not already -- the panel controls
  gpbridge.service, so that unit needs to exist.
DONE
