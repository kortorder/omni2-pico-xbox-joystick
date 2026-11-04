#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/omni-nudges.XXXXXX")
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DOMNI_USB_VID=0x1209 -DOMNI_USB_PID=2 -DOMNI_USB_POWER_MA=500 -I"$here/shim" -I"$here/../src" \
  "$here/../src/omni_nudges.cpp" "$here/../src/omni_stick.cpp" "$here/../src/omni_usb.cpp" \
  "$here/omni_nudge_test.cpp" -o "$build/test"
"$build/test"
