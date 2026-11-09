#!/bin/bash
# configure-wine.sh <64|32> -- configure one Wine build tree under build/. The 64-bit tree is the
# x86_64 host side with x86_64 PE modules; the 32-bit tree is the i386 host side with i386 PE modules
# and points at the 64-bit tree (--with-wine64), which together form a WoW64 installation.
#
# The installation lands in a wrapper app's Contents/Resources/Engine. Wine finds its directories
# relative to its own files, so the installed app can live anywhere.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/env.sh"
BITS="$1"
case "$BITS" in
    64) ARCH=x86_64; EXTRA=(--enable-win64) ;;
    32) ARCH=i386;   EXTRA=(--with-wine64="$WS_REPO/build/wine64") ;;
    *) echo "usage: $0 <64|32>" >&2; exit 2 ;;
esac
TREE="$WS_REPO/build/wine$BITS"
# A configure starts a fresh tree: objects carry configure's results in compiler defines the Makefile
# rules do not depend on. ccache makes the rebuild cheap.
rm -rf "$TREE"
POLY="$WS_SUPPORT/polyfill"
[ -f "$POLY/build/link-$ARCH.rsp" ] || "$POLY/build-polyfill.sh"
POLY_CFLAGS="-isystem $POLY/build/include -isystem $POLY/include"
mkdir -p "$TREE"
cd "$TREE"
export PKG_CONFIG_PATH="$WS_DEPS/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$WS_DEPS/lib/pkgconfig"
# winegstreamer.so and winedmo.so link the media runtime (deps/build_media.sh), which the engine holds
# in lib/, two levels above lib/wine/<arch>-unix.
"$WS_REPO/configure" \
    --prefix="$WS_APP_ROOT/Contents/Resources/Engine" \
    --host=$ARCH-apple-darwin \
    "${EXTRA[@]}" \
    CC="$WS_CCACHE $WS_HOST_CC $(ws_host_cflags $ARCH)" \
    OBJC="$WS_CCACHE $WS_HOST_CC $(ws_host_cflags $ARCH)" \
    CFLAGS="-O2 -g $POLY_CFLAGS" OBJCFLAGS="-O2 -g $POLY_CFLAGS" \
    LDFLAGS="$(ws_host_ldflags $ARCH) @$POLY/build/link-$ARCH.rsp" \
    ${ARCH}_CC="$WS_CCACHE $WS_CLANG" \
    CROSSCFLAGS="-O2 -g" \
    --with-freetype \
    FREETYPE_CFLAGS="-I$WS_DEPS/include/freetype2" \
    FREETYPE_LIBS="-L$WS_DEPS/lib -lfreetype -Wl,-rpath,$WS_DEPS/lib" \
    --with-gnutls \
    --with-gstreamer \
    GSTREAMER_LIBS="$(pkg-config --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0) -Wl,-rpath,@loader_path/../.." \
    --with-ffmpeg \
    FFMPEG_LIBS="$(pkg-config --libs libavutil libavformat libavcodec) -Wl,-rpath,@loader_path/../.." \
    --with-coreaudio \
    --without-x --without-wayland --without-cups --without-sane \
    --without-gphoto --without-pcap --without-usb --without-v4l2 --without-dbus \
    --without-krb5 --without-netapi --without-oss --without-pulse --without-alsa \
    --without-udev --without-capi --without-inotify --without-vulkan \
    --without-sdl --without-fontconfig
