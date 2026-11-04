#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/omni-settings.XXXXXX")
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$here/shim" -I"$here/../src" "$here/../src/omni_nudges.cpp" "$here/../src/omni_nudge_settings.cpp" "$here/nudge_settings_test.cpp" -o "$work/settings"
"$work/settings"
