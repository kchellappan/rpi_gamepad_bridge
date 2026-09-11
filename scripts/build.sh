#!/bin/bash
# Build gpbridge and gpb-discover. Needs no root and no libraries.
#
# Prefers cmake, but falls back to calling g++ directly so that a bare image with only a
# compiler can still build this. That fallback is not a nicety: it is what let this build
# on a freshly flashed Pi before apt had been touched.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"

CORE_SRC=(
  src/config.cpp src/rt.cpp src/transform.cpp src/recorder.cpp
  src/bridge.cpp src/factory.cpp
  src/sources/evdev_source.cpp src/sources/socket_source.cpp
  src/sinks/ns_hid_sink.cpp
)

# USE_CMAKE=0 forces the fallback. CI uses it to keep that path exercised on runners that
# do have cmake -- shadowing the binary does not work, since the fallback is selected by
# `command -v` finding nothing rather than by cmake failing.
if [[ "${USE_CMAKE:-1}" != "0" ]] && command -v cmake >/dev/null 2>&1; then
  echo "==> cmake build (${BUILD_TYPE}, -j${JOBS})"
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null
  cmake --build "$BUILD_DIR" -j"$JOBS"
else
  echo "==> cmake not found; falling back to direct g++"
  command -v g++ >/dev/null || { echo "no g++ either -- run scripts/install_deps.sh" >&2; exit 1; }
  mkdir -p "$BUILD_DIR"
  FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iinclude)
  echo "    gpbridge"
  g++ "${FLAGS[@]}" "${CORE_SRC[@]}" src/main.cpp     -o "$BUILD_DIR/gpbridge"   -lpthread
  echo "    gpb-discover"
  g++ "${FLAGS[@]}" "${CORE_SRC[@]}" tools/discover.cpp -o "$BUILD_DIR/gpb-discover" -lpthread
  echo "    gpb-fakepad"
  g++ "${FLAGS[@]}" tools/fake_pad.cpp -o "$BUILD_DIR/gpb-fakepad"
fi

echo
ls -la "$BUILD_DIR"/gpbridge "$BUILD_DIR"/gpb-discover "$BUILD_DIR"/gpb-fakepad
echo
echo "next: ./$BUILD_DIR/gpb-discover list"
