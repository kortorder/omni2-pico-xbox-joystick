#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/omni-stick-usb.XXXXXX")
trap 'rm -rf -- "$build"' EXIT
# The second pair is synthetic and used only in a host-memory descriptor.
for identity in "0x1209 2" "0x1234 0x5678"; do
  set -- $identity
  c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DOMNI_USB_VID="$1" -DOMNI_USB_PID="$2" -DOMNI_USB_POWER_MA=500 -I"$here/shim" -I"$here/../src" \
    "$here/../src/omni_mouse_report.cpp" "$here/../src/omni_stick.cpp" "$here/../src/omni_nudges.cpp" "$here/../src/omni_usb.cpp" \
    "$here/omni_stick_usb_test.cpp" -o "$build/test"
  "$build/test"
done
