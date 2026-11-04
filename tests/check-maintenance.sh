#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/omni-maintenance.XXXXXX")
for mode in standard capture pinned; do
  capture=0; pinned=0
  case "$mode" in capture) capture=1;; pinned) pinned=1;; esac
  c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DOMNI_CAPTURE_SHORT_KEY="$capture" -DOMNI_SHORT_KEY_COMPAT="$pinned" -DOMNI_USB_VID=0x1209 -DOMNI_USB_PID=2 -DOMNI_USB_POWER_MA=500 \
    -I"$here/shim" -I"$here/../src" "$here/../src/omni_stick.cpp" "$here/../src/omni_nudges.cpp" "$here/../src/omni_nudge_settings.cpp" "$here/../src/omni_usb.cpp" \
    "$here/../src/omni_maintenance.cpp" "$here/omni_maintenance_test.cpp" -o "$build/test-$mode"
  echo "Firmware mode: $mode"
  "$build/test-$mode"
done
