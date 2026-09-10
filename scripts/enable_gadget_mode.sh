#!/bin/bash
# Switch the Pi's USB-C port between host and peripheral (gadget) mode. Requires a reboot.
#
# Why this is needed: a freshly flashed Raspberry Pi OS image ships config.txt containing
#   otg_mode=1
#   dtoverlay=dwc2,dr_mode=host
# so dwc2 is loaded but bound as a HOST. /sys/class/udc is then empty and gadget_up.sh has
# nothing to bind to. The fix is one word -- host -> peripheral -- but it is not obvious
# from the symptom, which is simply "no UDC found".
#
#   enable_gadget_mode.sh            switch to peripheral (gadget) mode
#   enable_gadget_mode.sh --host     switch back to host mode
#   enable_gadget_mode.sh --show     print the current state and exit
set -euo pipefail

CONFIG="${CONFIG:-/boot/firmware/config.txt}"
CMDLINE="${CMDLINE:-/boot/firmware/cmdline.txt}"
MODE="peripheral"

case "${1:-}" in
  --host)  MODE="host" ;;
  --show)  MODE="show" ;;
  "")      ;;
  *)       echo "usage: $0 [--host|--show]" >&2; exit 2 ;;
esac

[[ -f "$CONFIG" ]] || { echo "no such file: $CONFIG" >&2; exit 1; }

show_state() {
  echo "--- $CONFIG ---"
  grep -nE '^[[:space:]]*(otg_mode|dtoverlay=dwc2)' "$CONFIG" || echo "  (no dwc2/otg_mode lines)"
  echo "--- current UDC ---"
  if compgen -G "/sys/class/udc/*" >/dev/null; then
    for u in /sys/class/udc/*; do
      echo "  $(basename "$u")  state=$(cat "$u/state" 2>/dev/null || echo ?)  speed=$(cat "$u/current_speed" 2>/dev/null || echo ?)"
    done
  else
    echo "  (none -- not in peripheral mode, or not yet rebooted)"
  fi
}

if [[ "$MODE" == "show" ]]; then show_state; exit 0; fi

if [[ $EUID -ne 0 ]]; then echo "must run as root (sudo $0 $*)" >&2; exit 1; fi

BACKUP="${CONFIG}.bak.$(date +%Y%m%d-%H%M%S)"
cp -a "$CONFIG" "$BACKUP"
echo "==> backed up to $BACKUP"

if grep -qE '^[[:space:]]*dtoverlay=dwc2' "$CONFIG"; then
  # Rewrite whatever dr_mode the line currently carries (including none at all).
  sed -i -E "s|^([[:space:]]*)dtoverlay=dwc2.*$|\1dtoverlay=dwc2,dr_mode=${MODE}|" "$CONFIG"
  echo "==> rewrote existing dwc2 overlay line -> dr_mode=${MODE}"
else
  printf '\n# added by rpi_gamepad_bridge\ndtoverlay=dwc2,dr_mode=%s\n' "$MODE" >> "$CONFIG"
  echo "==> appended dtoverlay=dwc2,dr_mode=${MODE}"
fi

# The overlay loads the module itself; having modules-load=dwc2 as well has been reported
# to conflict. Report it rather than silently editing the kernel command line.
if [[ -f "$CMDLINE" ]] && grep -q 'modules-load=dwc2' "$CMDLINE"; then
  echo "WARNING: $CMDLINE also contains modules-load=dwc2."
  echo "         The overlay already loads it; consider removing that token."
fi

echo
show_state
echo
echo "==> reboot required for this to take effect:  sudo reboot"
