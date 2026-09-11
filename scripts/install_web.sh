#!/bin/bash
# Install and configure the web control panel.
#
# Grants the web user exactly the privileges it needs through sudoers -- restarting the two
# units and reading the bridge's journal -- rather than running the server as root. The panel
# can stop a service and swap configs on a device plugged into a console, so it is worth
# keeping its reach narrow and legible.
#
#   install_web.sh [--port N] [--user NAME]   install, enable and start
#   install_web.sh --set-password [VALUE]     set the password, or generate one if omitted
#   install_web.sh --set-user NAME            change the username
#   install_web.sh --show                     print the current URL and username
#   install_web.sh --uninstall                remove the service, sudoers rule and secrets
#
# Credentials are read from disk on every request, so --set-password and --set-user take
# effect immediately and do not need a restart.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR=/etc/systemd/system
CONF_DIR=/etc/gpbridge
STATE_DIR=/var/lib/gpbridge
ENVFILE="$STATE_DIR/active.env"
PASSFILE="$CONF_DIR/webpass"
USERFILE="$CONF_DIR/webuser"
SUDOERS=/etc/sudoers.d/020_gpb-web
SERVICE_USER="${SUDO_USER:-${USER:-pi}}"

PORT=8080
WEB_USER=""
MODE=install
NEW_SECRET=""

need_root() { [[ $EUID -eq 0 ]] || { echo "must run as root (sudo $0 $*)" >&2; exit 1; }; }

gen_password() { python3 -c "import secrets; print(secrets.token_urlsafe(18))"; }

# Owned by root, readable by the service account, invisible to everyone else.
write_secret() {
  local path="$1" value="$2"
  printf '%s\n' "$value" > "$path"
  chmod 640 "$path"
  chown root:"$SERVICE_USER" "$path"
}

show() {
  local ip; ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  local port; port="$(sed -n 's/^Environment=GPB_PORT=//p' "$UNIT_DIR/gpb-web.service" 2>/dev/null | tail -1)"
  cat <<SHOW

  Control panel:  http://$(hostname).local:${port:-$PORT}/
                  http://${ip:-127.0.0.1}:${port:-$PORT}/

  Username:       $(cat "$USERFILE" 2>/dev/null || echo admin)
  Password:       $(cat "$PASSFILE" 2>/dev/null || echo '(none set -- auth is DISABLED)')
SHOW
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)          PORT="$2"; shift 2 ;;
    --user)          WEB_USER="$2"; shift 2 ;;
    --set-password)  MODE=set-password; NEW_SECRET="${2:-}"; shift; [[ $# -gt 0 ]] && shift || true ;;
    --set-user)      MODE=set-user; NEW_SECRET="$2"; shift 2 ;;
    --show)          MODE=show; shift ;;
    --uninstall)     MODE=uninstall; shift ;;
    [0-9]*)          PORT="$1"; shift ;;          # legacy positional port
    -h|--help)       sed -n '2,20p' "$0"; exit 0 ;;
    *)               echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$MODE" in
  show)
    show
    exit 0 ;;

  uninstall)
    need_root
    systemctl disable --now gpb-web.service 2>/dev/null || true
    rm -f "$UNIT_DIR/gpb-web.service" "$SUDOERS" "$PASSFILE" "$USERFILE"
    systemctl daemon-reload
    echo "removed. gpbridge and gpb-gadget are untouched."
    exit 0 ;;

  set-password)
    need_root
    mkdir -p "$CONF_DIR"
    [[ -n "$NEW_SECRET" ]] || { NEW_SECRET="$(gen_password)"; echo "==> generated a new password"; }
    write_secret "$PASSFILE" "$NEW_SECRET"
    echo "==> password updated (takes effect immediately; no restart needed)"
    show
    exit 0 ;;

  set-user)
    need_root
    mkdir -p "$CONF_DIR"
    write_secret "$USERFILE" "$NEW_SECRET"
    echo "==> username updated (takes effect immediately; no restart needed)"
    show
    exit 0 ;;
esac

# ---------------------------------------------------------------------------- install
need_root
[[ -x "$REPO/build/gpbridge" ]] || { echo "build first: $REPO/scripts/build.sh" >&2; exit 1; }
mkdir -p "$CONF_DIR" "$STATE_DIR"
# The service writes its selection here by creating a temp file and renaming, so it needs
# write permission on the directory itself, not just on the file.
chown "$SERVICE_USER" "$STATE_DIR"
# Seed the active selection if absent, so the bridge has something to start with before
# anyone opens the page.
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
chown "$SERVICE_USER" "$ENVFILE"

# Do not regenerate secrets that already exist: reinstalling would otherwise silently
# invalidate a saved bookmark's credentials.
if [[ -n "$WEB_USER" ]]; then
  write_secret "$USERFILE" "$WEB_USER"
  echo "==> username set to $WEB_USER"
elif [[ ! -f "$USERFILE" ]]; then
  write_secret "$USERFILE" "admin"
fi
if [[ ! -f "$PASSFILE" ]]; then
  write_secret "$PASSFILE" "$(gen_password)"
  echo "==> generated a web password"
fi

cat > "$SUDOERS" <<SUDO
# Installed by rpi_gamepad_bridge/scripts/install_web.sh
# The web control panel runs unprivileged and needs exactly these.
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/systemctl start gpbridge.service
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/systemctl stop gpbridge.service
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/systemctl restart gpbridge.service
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/systemctl restart gpb-gadget.service
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/journalctl -u gpbridge.service *
SUDO
chmod 440 "$SUDOERS"
# A malformed sudoers file can lock the machine out of sudo entirely, so validate it and
# remove it rather than leaving it in place.
if ! visudo -cf "$SUDOERS" >/dev/null; then
  rm -f "$SUDOERS"
  echo "generated sudoers rule failed validation; removed it" >&2
  exit 1
fi
echo "==> sudoers rule installed and validated ($SUDOERS)"

sed -e "s|@REPO@|$REPO|g" -e "s|@USER@|$SERVICE_USER|g" \
    -e "s|@ENVFILE@|$ENVFILE|g" -e "s|@PORT@|$PORT|g" \
    "$REPO/systemd/gpb-web.service.in" > "$UNIT_DIR/gpb-web.service"

systemctl daemon-reload
systemctl enable gpb-web.service >/dev/null
systemctl restart gpb-web.service
show
cat <<'DONE'
  Change them later with:
      sudo ./scripts/install_web.sh --set-password [value]
      sudo ./scripts/install_web.sh --set-user NAME

  Run scripts/install_services.sh as well if you have not already -- the panel controls
  gpbridge.service, so that unit needs to exist.
DONE
