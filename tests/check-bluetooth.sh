#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d ${TMPDIR:-/tmp}/omni-bt-checks-XXXXXX)"
opts=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -I "$root/src")
clang++ "${opts[@]}" "$root/tests/mouse_report_checks.cpp" "$root/src/omni_mouse_report.cpp" -o "$work/reports"
"$work/reports"
bt="${PICO_BTSTACK_PATH:-${PICO_SDK_PATH:?Set PICO_SDK_PATH to the pinned Pico SDK}/lib/btstack}"
clang++ "${opts[@]}" -DENABLE_CLASSIC=1 -isystem "$bt/src" "$root/tests/bluetooth_checks.cpp" "$root/src/omni_bluetooth.cpp" "$root/src/omni_mouse_report.cpp" -o "$work/lifecycle"
for scenario in pair inbound missing window timeout diagnostic diagnostic_timeout security_status key_length provisional expiry forget_provisional bad_descriptor interrupted promotion_interrupted capture badcapture strict_floor telemetry_unavailable; do "$work/lifecycle" "$scenario"; done

# The same transport/event tests run with the private observation endpoint enabled.
clang++ "${opts[@]}" -DENABLE_CLASSIC=1 -DOMNI_CAPTURE_SHORT_KEY=1 -isystem "$bt/src" "$root/tests/bluetooth_checks.cpp" "$root/src/omni_bluetooth.cpp" "$root/src/omni_mouse_report.cpp" -o "$work/capture"
for scenario in capture badcapture capture_reset capture_dead_acl strict_floor telemetry_unavailable; do "$work/capture" "$scenario"; done

# Synthetic address, no captured chair identity is read by these tests.
printf '#define OMNI_PINNED_MOUSE2_BYTES 1,2,3,4,5,6\n' > "$work/omni_approved_peer.hpp"
clang++ "${opts[@]}" -DENABLE_CLASSIC=1 -DOMNI_SHORT_KEY_COMPAT=1 -DENABLE_MUTUAL_AUTHENTICATION_FOR_LEGACY_SECURE_CONNECTIONS=1 -I "$work" -isystem "$bt/src" "$root/tests/bluetooth_checks.cpp" "$root/src/omni_bluetooth.cpp" "$root/src/omni_mouse_report.cpp" -o "$work/pinned"
for scenario in pinned pinned_weak pinned_debug pinned_saved capture badcapture telemetry telemetry_security telemetry_framing; do "$work/pinned" "$scenario"; done
