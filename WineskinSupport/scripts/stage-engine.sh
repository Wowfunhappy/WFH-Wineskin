#!/bin/bash
# stage-engine.sh -- install the built Wine trees into build/engine, the skeleton of a wrapper app
# (Contents/Resources/Engine): the 64-bit tree first, then the 32-bit tree over it, plus the third-party
# libraries Wine loads at runtime and wine-mono.
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
cp "$WS_DEPS/lib/libfreetype.6.dylib" "$WS_DEPS/lib/libgnutls.30.dylib" "$ENGINE/Contents/Resources/Engine/lib/"
MONO_VERSION="$(sed -n 's/^#define WINE_MONO_VERSION "\(.*\)"$/\1/p' "$WS_REPO/dlls/mscoree/mscoree_private.h")"
MONO_TARBALL="$WS_SUPPORT/deps/work/src/wine-mono-$MONO_VERSION-x86.tar.xz"
[ -f "$MONO_TARBALL" ] || { echo "no $MONO_TARBALL; run deps/build_deps.sh" >&2; exit 1; }
# Mozilla's root certificates (deps/ca/cacert.pem), which crypt32 adds to the Mac's.
cp "$WS_SUPPORT/deps/ca/cacert.pem" "$ENGINE/Contents/Resources/Engine/share/wine/"
mkdir -p "$ENGINE/Contents/Resources/Engine/share/wine/mono"
tar xJf "$MONO_TARBALL" -C "$ENGINE/Contents/Resources/Engine/share/wine/mono"
# The media runtime (deps/build_media.sh): its libraries in lib/, GStreamer's plugins in
# lib/gstreamer-1.0 and its plugin scanner in libexec/gstreamer-1.0, where libgstreamer looks for them.
MEDIA="$WS_DEPS/lib/media-runtime.txt"
[ -f "$MEDIA" ] || { echo "no $MEDIA; run deps/build_deps.sh" >&2; exit 1; }
while read -r lib; do
    cp -L "$WS_DEPS/lib/$lib" "$ENGINE/Contents/Resources/Engine/lib/$lib"
done < "$MEDIA"
cp -R "$WS_DEPS/lib/gstreamer-1.0" "$ENGINE/Contents/Resources/Engine/lib/"
mkdir -p "$ENGINE/Contents/Resources/Engine/libexec/gstreamer-1.0"
cp "$WS_DEPS/libexec/gstreamer-1.0/gst-plugin-scanner" "$ENGINE/Contents/Resources/Engine/libexec/gstreamer-1.0/"
du -sh "$ENGINE"
echo "### engine staged -> $ENGINE"
