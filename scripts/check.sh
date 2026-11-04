#!/usr/bin/env bash
# Compile and run host tests. No board, Bluetooth radio or USB connection is needed.
set -euo pipefail
: "${PICO_SDK_PATH:?Set PICO_SDK_PATH to an external Pico SDK 2.3.1 checkout.}"
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONDONTWRITEBYTECODE=1
for suite in check-bluetooth check-stick-usb check-nudges check-nudge-settings check-maintenance check-nudge-loop; do
    bash "$root/tests/$suite.sh"
done
python3 "$root/tests/private_pin_checks.py"
