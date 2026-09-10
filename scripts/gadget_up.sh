#!/bin/bash
# Bring up a USB HID gadget that presents as a HORI Pokken Tournament Pro Pad.
#
# The Switch accepts this device as a controller over plain USB HID with no authentication
# handshake, which is the entire reason the Switch is reachable and PS/Xbox consoles are
# not. Run as root, after `dtoverlay=dwc2,dr_mode=peripheral` is in /boot/firmware/config.txt.
set -euo pipefail

GADGET_NAME="${GADGET_NAME:-gpbpad}"
G="/sys/kernel/config/usb_gadget/${GADGET_NAME}"

if [[ $EUID -ne 0 ]]; then echo "must run as root" >&2; exit 1; fi

modprobe libcomposite
if [[ ! -d /sys/kernel/config/usb_gadget ]]; then
  echo "configfs gadget dir missing -- is libcomposite loaded and configfs mounted?" >&2
  exit 1
fi

UDC_NAME="$(ls /sys/class/udc 2>/dev/null | head -1 || true)"
if [[ -z "$UDC_NAME" ]]; then
  cat >&2 <<'MSG'
No UDC found. The USB controller is not in peripheral mode.

Check, in order:
  1. /boot/firmware/config.txt contains:  dtoverlay=dwc2,dr_mode=peripheral
  2. cmdline.txt does NOT also contain   modules-load=dwc2   (the overlay loads it;
     having both has been observed to conflict)
  3. You rebooted after changing config.txt
MSG
  exit 1
fi

if [[ -d "$G" ]]; then
  echo "gadget ${GADGET_NAME} already exists; tearing down first"
  "$(dirname "$0")/gadget_down.sh"
fi

mkdir -p "$G"
cd "$G"

echo 0x0f0d > idVendor          # HORI CO., LTD.
echo 0x0092 > idProduct         # Pokken Tournament Pro Pad
echo 0x0200 > bcdUSB            # USB 2.0
echo 0x0100 > bcdDevice

mkdir -p strings/0x409
echo "HORI CO.,LTD."      > strings/0x409/manufacturer
echo "POKKEN CONTROLLER"  > strings/0x409/product
echo "000000000001"       > strings/0x409/serialnumber

mkdir -p configs/c.1/strings/0x409
echo "Configuration 1" > configs/c.1/strings/0x409/configuration
echo 500               > configs/c.1/MaxPower

mkdir -p functions/hid.usb0
echo 0 > functions/hid.usb0/protocol
echo 0 > functions/hid.usb0/subclass
echo 8 > functions/hid.usb0/report_length

# HID report descriptor: 16 buttons, one 4-bit hat (+4 bits padding), four 8-bit axes,
# one constant vendor byte. Total 8 bytes, matching struct PokkenReport.
printf '%b' \
'\x05\x01\x09\x05\xa1\x01\x15\x00\x25\x01\x35\x00\x45\x01\x75\x01'\
'\x95\x10\x05\x09\x19\x01\x29\x10\x81\x02\x05\x01\x25\x07\x46\x3b'\
'\x01\x75\x04\x95\x01\x65\x14\x09\x39\x81\x42\x65\x00\x95\x01\x81'\
'\x01\x26\xff\x00\x46\xff\x00\x09\x30\x09\x31\x09\x32\x09\x35\x75'\
'\x08\x95\x04\x81\x02\x75\x08\x95\x01\x81\x01\xc0' \
> functions/hid.usb0/report_desc

ln -s functions/hid.usb0 configs/c.1/
echo "$UDC_NAME" > UDC

echo "gadget up on UDC ${UDC_NAME}; device node:"
ls -l /dev/hidg* 2>/dev/null || echo "  (no /dev/hidg* yet -- give udev a moment)"

cat <<'NOTE'

Note on polling interval: the f_hid gadget driver hardcodes the endpoint bInterval (there
is a long-standing FIXME in the kernel noting it is not exposed through configfs). At high
speed that works out to 1ms, which is already the value we would have asked for. So verify
the link actually enumerated at high speed rather than trying to tune the descriptor:

  cat /sys/class/udc/*/current_speed     # want "high-speed"

If it reports full-speed, the interval is far worse and that -- not anything in userspace
-- is the latency problem to chase.
NOTE
