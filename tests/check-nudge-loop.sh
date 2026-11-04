#!/bin/bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/omni-loop.XXXXXX")
trap 'rm -rf -- "$out"' EXIT
opts=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
    -DOMNI_USB_VID=0x1209 -DOMNI_USB_PID=2 -DOMNI_USB_POWER_MA=500
    -I"$here/loop_shim" -I"$here/shim" -I"$here/../src"
    -isystem "${PICO_SDK_PATH:?Set PICO_SDK_PATH to the pinned Pico SDK}/lib/btstack/src"
    -include "$here/loop_shim/platform.hpp")
for pinned in 0 1; do
    echo "Replaying firmware loop with OMNI_SHORT_KEY_COMPAT=$pinned"
    c++ "${opts[@]}" -DOMNI_SHORT_KEY_COMPAT="$pinned" -Dmain=firmware_loop_main \
        -c "$here/../src/main.cpp" -o "$out/main.o"
    c++ "${opts[@]}" -DOMNI_SHORT_KEY_COMPAT="$pinned" "$here/nudge_loop_replay.cpp" "$out/main.o" \
        "$here"/../src/{omni_stick,omni_usb,omni_nudges,omni_maintenance,omni_nudge_settings}.cpp -o "$out/test"
    "$out/test"
done
