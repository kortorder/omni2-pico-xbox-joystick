#!/usr/bin/env bash
# Build outside the source tree. USB identity and optional private inputs are caller selections.
set -euo pipefail
if (( $# == 0 )); then
    echo 'Usage: scripts/build.sh OUTPUT_DIRECTORY [CMake -D options]' >&2
    exit 2
fi
src=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
out=$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).expanduser().resolve())' "$1")
shift
case "$out" in "$src"|"$src"/*) echo 'Choose a build directory outside the source tree.' >&2; exit 2;; esac
: "${PICO_SDK_PATH:?Set PICO_SDK_PATH to an external Pico SDK 2.3.1 checkout.}"
umask 077
cmake -S "$src" -B "$out" -G Ninja -DPICO_BOARD=pico_w "-DPICO_SDK_PATH=$PICO_SDK_PATH" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$out" --parallel
printf 'Firmware: %s/omni_right_stick.uf2\n' "$out"
