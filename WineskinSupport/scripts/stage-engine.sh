#!/bin/bash
# stage-engine.sh -- install the built Wine trees into build/engine, the skeleton of a wrapper app
# (Contents/Resources/Engine): the 64-bit tree first, then the 32-bit tree over it, plus the third-party
# libraries Wine loads at runtime.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/env.sh"
DEST="$WS_REPO/build/engine-dest"
ENGINE="$WS_REPO/build/engine"
echo "### stage engine"
rm -rf "$DEST" "$ENGINE"
make -C "$WS_REPO/build/wine64" -j"$WS_JOBS" install-lib DESTDIR="$DEST"
make -C "$WS_REPO/build/wine32" -j"$WS_JOBS" install-lib DESTDIR="$DEST"
mv "$DEST$WS_APP_ROOT" "$ENGINE"
rm -rf "$DEST"
cp "$WS_DEPS/lib/libfreetype.6.dylib" "$ENGINE/Contents/Resources/Engine/lib/"
du -sh "$ENGINE"
echo "### engine staged -> $ENGINE"
