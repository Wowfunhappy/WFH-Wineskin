#!/bin/bash
# Build the third-party libraries Wine's Unix side loads into deps/build (gitignored), for both host
# architectures. Each library is built once per architecture into deps/work/<arch> and the results are
# merged with lipo into deps/build, a universal (i386 + x86_64) prefix. Idempotent per package.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/build-log.sh"
build_log_open
. "$HERE/../scripts/env.sh"
WORK="$HERE/work"
SRC="$WORK/src"
OUT="$HERE/build"
mkdir -p "$SRC" "$OUT/lib" "$OUT/include"

fetch() {  # <url>
    local t="$SRC/$(basename "$1")"
    [ -f "$t" ] || { curl -fsSL --retry 5 --connect-timeout 30 --speed-time 60 --speed-limit 1000 -o "$t.part" "$1" && mv "$t.part" "$t"; }
    echo "$t"
}

# build_autotools <name> <version> <url> <dylib> [configure args...]
build_autotools() {
    local name="$1" ver="$2" url="$3" dylib="$4"; shift 4
    if [ -f "$OUT/lib/$dylib" ]; then echo "### $name: present"; return; fi
    local tarball; tarball="$(fetch "$url")"
    for arch in i386 x86_64; do
        echo "### $name $ver ($arch)"
        local b="$WORK/$arch/build/$name"
        rm -rf "$b"; mkdir -p "$b"
        tar xzf "$tarball" -C "$b"
        ( cd "$b/$name-$ver"
          CC="$WS_HOST_CC $(ws_host_cflags $arch)" CFLAGS="-O2" LDFLAGS="$(ws_host_ldflags $arch)" \
              ./configure --prefix="$WORK/$arch/prefix" --host=$arch-apple-darwin --build=x86_64-apple-darwin "$@"
          make -j"$WS_JOBS"
          make install )
    done
    # Headers come from the x86_64 install; the libraries are merged across both.
    cp -R "$WORK/x86_64/prefix/include/." "$OUT/include/"
    lipo -create "$WORK/i386/prefix/lib/$dylib" "$WORK/x86_64/prefix/lib/$dylib" -output "$OUT/lib/$dylib"
    install_name_tool -id "@rpath/$dylib" "$OUT/lib/$dylib"
    ln -sf "$dylib" "$OUT/lib/${dylib%%.*}.dylib"
    echo "### $name OK -> $OUT/lib/$dylib"
}

build_autotools freetype 2.13.3 https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz \
    libfreetype.6.dylib --without-png --without-harfbuzz --without-brotli --without-bzip2 --enable-shared --disable-static
# Wine reads FreeType's headers through pkg-config; one .pc file serves both architectures.
mkdir -p "$OUT/lib/pkgconfig"
sed "s|$WORK/x86_64/prefix|$OUT|g" "$WORK/x86_64/prefix/lib/pkgconfig/freetype2.pc" > "$OUT/lib/pkgconfig/freetype2.pc"

# wine-mono, which mscoree finds unpacked under the engine's share/wine/mono (stage-engine.sh): .NET
# programs run without Wine offering to download it. The version is the one dlls/mscoree expects.
MONO_VERSION="$(sed -n 's/^#define WINE_MONO_VERSION "\(.*\)"$/\1/p' "$WS_REPO/dlls/mscoree/mscoree_private.h")"
fetch "https://dl.winehq.org/wine/wine-mono/$MONO_VERSION/wine-mono-$MONO_VERSION-x86.tar.xz" >/dev/null
echo "### wine-mono $MONO_VERSION present"

echo "### deps ready under $OUT"
