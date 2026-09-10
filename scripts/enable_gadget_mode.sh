#!/bin/bash
# Switch the Pi's USB-C port between host and peripheral (gadget) mode. Requires a reboot.
#
# Why this is needed: a freshly flashed Raspberry Pi OS image ships config.txt with dwc2
# already configured -- but scoped to a conditional section that may not apply to your
# board. On a Pi 5 Model B the stock file contains:
#
#     [cm4]
#     otg_mode=1
#     [cm5]
#     dtoverlay=dwc2,dr_mode=host      <-- only applies to Compute Module 5
#     [pi5]
#     [all]
#
# so dwc2 is never enabled on a Pi 5 Model B, /sys/class/udc stays empty, and gadget_up.sh
# fails with "no UDC found". Editing that line in place does nothing, because the line was
# never in scope. The overlay has to be written into a section this board actually reads.
#
# This script only ever treats "[all]" and the pre-section preamble as in-scope, and writes
# there. Lines in other sections are left alone (they are for other boards) but are
# reported, because finding one is otherwise deeply confusing.
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

# Emits "lineno<TAB>section<TAB>text" for every dwc2 overlay line, with the section it
# falls under ("" meaning the preamble before any section header).
scan_dwc2() {
  awk '
    /^[[:space:]]*\[/ { s=$0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/,"",s); section=s; next }
    /^[[:space:]]*dtoverlay=dwc2/ { printf "%d\t%s\t%s\n", NR, section, $0 }
  ' "$CONFIG"
}

in_scope() { [[ -z "$1" || "$1" == "all" ]]; }

show_state() {
  echo "--- $CONFIG ---"
  local found=0
  while IFS=$'\t' read -r ln sec txt; do
    found=1
    if in_scope "$sec"; then
      printf '  line %-4s [%s]  %s   <-- in scope\n' "$ln" "${sec:-preamble}" "$txt"
    else
      printf '  line %-4s [%s]  %s   (ignored on this board)\n' "$ln" "$sec" "$txt"
    fi
  done < <(scan_dwc2)
  [[ $found -eq 1 ]] || echo "  (no dtoverlay=dwc2 lines at all)"

  echo "--- board ---"
  if [[ -r /proc/device-tree/model ]]; then
    echo "  $(tr -d '\0' < /proc/device-tree/model)"
  else
    echo "  (unknown)"
  fi
  echo "--- current UDC ---"
  if compgen -G "/sys/class/udc/*" >/dev/null 2>&1; then
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

# Rewrite an in-scope line if one exists; otherwise append a fresh [all] stanza. Appending
# our own [all] header is what makes this correct regardless of which section the file
# happened to end in.
TARGET_LINE=""
while IFS=$'\t' read -r ln sec txt; do
  if in_scope "$sec"; then TARGET_LINE="$ln"; fi
  if ! in_scope "$sec"; then
    printf 'NOTE: line %s is inside [%s] and does not apply to this board; leaving it alone.\n' "$ln" "$sec"
  fi
done < <(scan_dwc2)

if [[ -n "$TARGET_LINE" ]]; then
  sed -i -E "${TARGET_LINE}s|^([[:space:]]*)dtoverlay=dwc2.*$|\1dtoverlay=dwc2,dr_mode=${MODE}|" "$CONFIG"
  echo "==> rewrote in-scope overlay at line ${TARGET_LINE} -> dr_mode=${MODE}"
else
  printf '\n[all]\n# added by rpi_gamepad_bridge\ndtoverlay=dwc2,dr_mode=%s\n' "$MODE" >> "$CONFIG"
  echo "==> appended an [all] stanza with dtoverlay=dwc2,dr_mode=${MODE}"
fi

if [[ -f "$CMDLINE" ]] && grep -q 'modules-load=dwc2' "$CMDLINE"; then
  echo "WARNING: $CMDLINE also contains modules-load=dwc2."
  echo "         The overlay already loads it; consider removing that token."
fi

echo
show_state
echo
echo "==> reboot required for this to take effect:  sudo reboot"
