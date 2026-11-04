#!/bin/bash
# Build and verify locally. Offline self-checks never enumerate USB devices.
set -euo pipefail
umask 077
here=$(cd -- "$(dirname -- "$0")" && pwd)
out="$here/build"
usage() {
  echo "Usage: ./build.sh (--private-test-id | --usb-id VID:PID) [--output DIRECTORY]" >&2
  echo "VID and PID are hexadecimal, for example 1234:00AB. Use the IDs from your firmware build." >&2
  exit 2
}
[[ $# -ge 1 ]] || usage
case "$1" in
  --private-test-id)
    # pid.codes 1209:0002 is shared, for private in-house testing only.
    vendor=1209; product=0002; shift
    echo "Using the shared in-house test USB identity. Do not use it for distributed devices."
    ;;
  --usb-id)
    [[ $# -ge 2 && "$2" =~ ^([[:xdigit:]]{1,4}):([[:xdigit:]]{1,4})$ ]] || usage
    vendor=${BASH_REMATCH[1]}; product=${BASH_REMATCH[2]}; shift 2
    if ((16#$vendor == 0 || 16#$product == 0)); then usage; fi
    if ((16#$vendor == 0x1209 && 16#$product == 2)); then
      echo "The shared test identity requires the explicit --private-test-id option." >&2
      exit 2
    fi
    ;;
  *) usage ;;
esac
if [[ $# -gt 0 ]]; then
  [[ $# -eq 2 && "$1" == --output && -n "$2" ]] || usage
  out=$2; shift 2
fi
[[ -f "$here/../src/omni_maintenance.hpp" ]] || {
  echo "The firmware protocol header is missing. Use the mac folder from the complete chair-bridge repository." >&2
  exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$(mktemp -d "${TMPDIR:-/tmp}/omni-mac-build.XXXXXX")
trap 'rm -rf "$stage"' EXIT
app="$stage/Omni Nudge Editor.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$stage/cache"
cd "$here"
cxx=(-std=c++17 -O2 -Wall -Wextra -Werror "-DOMNI_USB_VID=0x$vendor" "-DOMNI_USB_PID=0x$product" -I../src)
xcrun --sdk macosx clang++ "${cxx[@]}" omni_maintenance_cli.cpp -framework IOKit -framework CoreFoundation -o "$stage/omni-maintenance"
xcrun --sdk macosx clang++ "${cxx[@]}" omni_observer.cpp -framework IOKit -framework CoreFoundation -o "$stage/omni-observer"
xcrun --sdk macosx swiftc -swift-version 5 -parse-as-library -O -module-cache-path "$stage/cache" \
  OmniNudgeEditor.swift -framework AppKit -o "$app/Contents/MacOS/OmniNudgeEditor"
install -m 755 "$stage/omni-maintenance" "$app/Contents/Helpers/omni-maintenance"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleExecutable</key><string>OmniNudgeEditor</string>
<key>CFBundleIdentifier</key><string>io.github.kortorder.chairbridge.omninudgeeditor</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleName</key><string>Omni Nudge Editor</string>
<key>CFBundleDisplayName</key><string>Omni Nudge Editor</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>0.2.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
plutil -lint "$app/Contents/Info.plist"
"$app/Contents/MacOS/OmniNudgeEditor" --self-test
"$app/Contents/Helpers/omni-maintenance" --self-test
"$stage/omni-observer" --self-test
python3 compare-chair-motion.py --self-test
codesign --force --sign - --timestamp=none "$app/Contents/Helpers/omni-maintenance"
codesign --force --sign - --timestamp=none "$app"
codesign --verify --deep --strict "$app"
ditto --noextattr --norsrc -c -k --keepParent "$app" "$stage/Omni Nudge Editor.zip"
mkdir "$stage/verify"
ditto --noextattr --norsrc -x -k "$stage/Omni Nudge Editor.zip" "$stage/verify"
codesign --verify --deep --strict "$stage/verify/Omni Nudge Editor.app"
install -m 755 "$stage/omni-maintenance" "$out/omni-maintenance"
install -m 755 "$stage/omni-observer" "$out/omni-observer"
install -m 644 "$stage/Omni Nudge Editor.zip" "$out/Omni Nudge Editor.zip"
printf 'Built and verified for USB %s:%s. Output: %s\n' "$vendor" "$product" "$out"
printf 'Extract Omni Nudge Editor.zip into your personal Applications folder to use the local app.\n'
