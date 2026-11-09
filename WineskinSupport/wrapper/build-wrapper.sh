#!/bin/bash
# build-wrapper.sh -- build the launcher and assemble build/WFH Wineskin.app around build/engine: a blank
# wrapper. Copy it, rename the copy after the program it will hold, and open it.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/build-log.sh"
build_log_open
. "$HERE/../scripts/env.sh"
ENGINE="$WS_REPO/build/engine"
APP="$WS_REPO/build/WFH Wineskin.app"
[ -x "$ENGINE/Contents/Resources/Engine/bin/wine" ] || { echo "no engine at $ENGINE; run build.sh first" >&2; exit 1; }
echo "### wrapper"
mkdir -p "$HERE/build"
"$WS_HOST_CC" $(ws_host_cflags x86_64) $(ws_host_ldflags x86_64) -fobjc-arc -O2 -Wall -Wno-deprecated-declarations \
    "$HERE/src/main.m" -framework Cocoa -framework ApplicationServices -o "$HERE/build/Wineskin"
sips -s format png "$WS_REPO/programs/winecfg/winecfg.ico" --out "$HERE/build/icon.png" >/dev/null
sips -s format icns "$HERE/build/icon.png" --out "$HERE/build/Wineskin.icns" >/dev/null

rm -rf "$APP"
ditto "$ENGINE" "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/SharedSupport"
cp "$HERE/build/Wineskin" "$APP/Contents/MacOS/Wineskin"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(cat "$WS_REPO/VERSION" | awk '{print $3}')" "$APP/Contents/Info.plist"
cp "$HERE/build/Wineskin.icns" "$APP/Contents/Resources/Wineskin.icns"
# The blank wrapper's icon, kept for a program that has none of its own.
cp "$HERE/build/Wineskin.icns" "$APP/Contents/Resources/Blank.icns"
echo "### wrapper assembled -> $APP"
