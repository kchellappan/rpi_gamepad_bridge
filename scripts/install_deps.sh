#!/bin/bash
# Install build dependencies. On-device builds only for now.
#
# The build itself needs only a C++20 compiler -- there are no library dependencies, by
# design, since the evdev source reads struct input_event off the character device rather
# than linking libevdev. cmake is a convenience; scripts/build.sh falls back to invoking
# g++ directly if it is missing, so this script is optional.
set -euo pipefail

SUDO=""
if [[ $EUID -ne 0 ]]; then
  if ! command -v sudo >/dev/null; then
    echo "not root and no sudo available" >&2; exit 1
  fi
  SUDO="sudo"
fi

echo "==> updating package lists"
$SUDO apt-get update -qq

echo "==> installing build dependencies"
$SUDO apt-get install -y --no-install-recommends build-essential cmake git

echo
echo "==> versions"
g++ --version | head -1
cmake --version | head -1
git --version

# Reading a controller without root needs group membership rather than sudo. Raspberry Pi
# OS puts the default user in 'input' already; say so plainly if it did not.
if ! id -nG | tr ' ' '\n' | grep -qx input; then
  echo
  echo "NOTE: $(id -un) is not in the 'input' group, so rgb-discover will need sudo."
  echo "      To fix (takes effect on next login):  $SUDO usermod -aG input $(id -un)"
fi
echo
echo "done. next: scripts/build.sh"
