#!/bin/bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/omni-relay.XXXXXX")
trap 'rm -rf -- "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DOMNI_PHONE_RELAY=1 -DOMNI_USB_VID=0x1209 -DOMNI_USB_PID=2 -DOMNI_USB_POWER_MA=500 \
  -I"$here/shim" -I"$here/../src" -I"$here/../src/relay_protocol" \
  "$here/phone_relay_usb_test.cpp" "$here/../src/omni_phone_relay.cpp" \
  "$here/../src/relay_protocol/scx2.cpp" "$here/../src/omni_usb.cpp" \
  "$here/../src/omni_stick.cpp" "$here/../src/omni_nudges.cpp" -o "$out/test"
"$out/test"
opts=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
  -DOMNI_PHONE_RELAY=1 -DOMNI_USB_VID=0x1209 -DOMNI_USB_PID=2 -DOMNI_USB_POWER_MA=500
  -I"$here/loop_shim" -I"$here/shim" -I"$here/../src" -I"$here/../src/relay_protocol"
  -isystem "${PICO_SDK_PATH:?Set PICO_SDK_PATH}/lib/btstack/src" -include "$here/loop_shim/platform.hpp")
for pinned in 0 1; do
  c++ "${opts[@]}" -DOMNI_SHORT_KEY_COMPAT="$pinned" -Dmain=firmware_loop_main -c "$here/../src/main.cpp" -o "$out/main.o"
  c++ "${opts[@]}" -DOMNI_SHORT_KEY_COMPAT="$pinned" "$here/phone_relay_loop_replay.cpp" "$out/main.o" \
    "$here"/../src/{omni_stick,omni_usb,omni_nudges,omni_maintenance,omni_nudge_settings,omni_phone_relay}.cpp \
    "$here/../src/relay_protocol/scx2.cpp" -o "$out/loop"
  "$out/loop"
done
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DOMNI_WIFI_CONFIG_HEADER='"wifi_test_config.h"' -I"$here/wifi_shim" -I"$here/shim" -I"$here/../src" \
  -I"$here/../src/relay_protocol" "$here/phone_wifi_test.cpp" "$here/../src/omni_phone_wifi.cpp" \
  "$here/../src/omni_phone_relay.cpp" "$here/../src/relay_protocol/scx2.cpp" -o "$out/wifi"
"$out/wifi"
"$out/wifi" --pm-failure
