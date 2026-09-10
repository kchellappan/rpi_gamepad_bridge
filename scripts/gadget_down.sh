#!/bin/bash
# Tear down the HID gadget. Order matters: unbind the UDC, unlink the function from the
# config, then remove directories bottom-up, or configfs refuses with EBUSY.
set -euo pipefail

GADGET_NAME="${GADGET_NAME:-gpbpad}"
G="/sys/kernel/config/usb_gadget/${GADGET_NAME}"

if [[ $EUID -ne 0 ]]; then echo "must run as root" >&2; exit 1; fi
[[ -d "$G" ]] || { echo "no gadget at $G"; exit 0; }

echo "" > "$G/UDC" 2>/dev/null || true
rm -f "$G/configs/c.1/hid.usb0"
rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null || true
rmdir "$G/configs/c.1" 2>/dev/null || true
rmdir "$G/functions/hid.usb0" 2>/dev/null || true
rmdir "$G/strings/0x409" 2>/dev/null || true
rmdir "$G" 2>/dev/null || true
echo "gadget ${GADGET_NAME} removed"
