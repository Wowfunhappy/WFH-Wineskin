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

# build_arch <name> <version> <tarball> <arch> [configure args...]: configure, make and install one
# package for one architecture into $WORK/<arch>/prefix. The packages already in that prefix are the
# ones it finds, by header and library path and through pkg-config.
build_arch() {
    local name="$1" ver="$2" tarball="$3" arch="$4"; shift 4
    echo "### $name $ver ($arch)"
    local b="$WORK/$arch/build/$name"
    rm -rf "$b"; mkdir -p "$b"
    tar xf "$tarball" -C "$b"
    local p="$WORK/$arch/prefix"
    ( cd "$b/$name-$ver"
      export PKG_CONFIG_PATH="$p/lib/pkgconfig" PKG_CONFIG_LIBDIR="$p/lib/pkgconfig"
      CC="$WS_HOST_CC $(ws_host_cflags $arch)" CFLAGS="-O2" CPPFLAGS="-I$p/include" \
      LDFLAGS="$(ws_host_ldflags $arch) -L$p/lib" \
          ./configure --prefix="$WORK/$arch/prefix" --host=$arch-apple-darwin --build=x86_64-apple-darwin "$@"
      make -j"$WS_JOBS"
      make install )
}

# build_autotools <name> <version> <url> <dylib> [configure args...]
build_autotools() {
    local name="$1" ver="$2" url="$3" dylib="$4"; shift 4
    if [ -f "$OUT/lib/$dylib" ]; then echo "### $name: present"; return; fi
    local tarball; tarball="$(fetch "$url")"
    for arch in i386 x86_64; do
        build_arch "$name" "$ver" "$tarball" "$arch" "$@"
    done
    # Headers come from the x86_64 install; the libraries are merged across both.
    cp -R "$WORK/x86_64/prefix/include/." "$OUT/include/"
    lipo -create "$WORK/i386/prefix/lib/$dylib" "$WORK/x86_64/prefix/lib/$dylib" -output "$OUT/lib/$dylib"
    install_name_tool -id "@rpath/$dylib" "$OUT/lib/$dylib"
    ln -sf "$dylib" "$OUT/lib/${dylib%%.*}.dylib"
    echo "### $name OK -> $OUT/lib/$dylib"
}

# build_static <name> <version> <url> <archive> [configure args...]: a static, position-independent
# library in each architecture's prefix, for a later package to link in. Nothing reaches deps/build.
build_static() {
    local name="$1" ver="$2" url="$3" archive="$4"; shift 4
    local tarball; tarball="$(fetch "$url")"
    for arch in i386 x86_64; do
        if [ -f "$WORK/$arch/prefix/lib/$archive" ]; then echo "### $name ($arch): present"; continue; fi
        build_arch "$name" "$ver" "$tarball" "$arch" --disable-shared --enable-static --with-pic "$@"
    done
}

build_autotools freetype 2.13.3 https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz \
    libfreetype.6.dylib --without-png --without-harfbuzz --without-brotli --without-bzip2 --enable-shared --disable-static
# Wine reads FreeType's headers through pkg-config; one .pc file serves both architectures.
mkdir -p "$OUT/lib/pkgconfig"
sed "s|$WORK/x86_64/prefix|$OUT|g" "$WORK/x86_64/prefix/lib/pkgconfig/freetype2.pc" > "$OUT/lib/pkgconfig/freetype2.pc"

# GnuTLS, the TLS implementation behind secur32's Schannel provider (and crypt32's PKCS #12 import),
# which Wine dlopens as libgnutls.30.dylib. Its crypto comes from nettle and hogweed over GMP, linked
# in statically: libgnutls.30.dylib is the one library of the set, and it needs only the system's.
# libtasn1 and libunistring are GnuTLS's included copies; PKCS #11, IDN, TPM, compression and the
# tools are left out. Certificate verification is crypt32's, against the Mac's trusted roots.
build_static gmp 6.3.0 https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz libgmp.a --disable-cxx
build_static nettle 3.10.2 https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz libhogweed.a \
    --disable-documentation --disable-openssl
if [ -f "$OUT/lib/libgnutls.30.dylib" ]; then echo "### gnutls: present"; else
    GNUTLS_TARBALL="$(fetch https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.13.tar.xz)"
    for arch in i386 x86_64; do
        build_arch gnutls 3.8.13 "$GNUTLS_TARBALL" "$arch" \
            --enable-shared --disable-static \
            --with-included-libtasn1 --with-included-unistring \
            --without-p11-kit --without-idn --without-tpm --without-tpm2 --without-zlib --without-brotli \
            --without-zstd --without-leancrypto --disable-tools --disable-doc --disable-manpages \
            --disable-tests --disable-nls --disable-cxx --disable-libdane --disable-openssl-compatibility \
            --with-default-trust-store-file=
    done
    mkdir -p "$OUT/include/gnutls"
    cp "$WORK/x86_64/prefix/include/gnutls/"*.h "$OUT/include/gnutls/"
    lipo -create "$WORK/i386/prefix/lib/libgnutls.30.dylib" "$WORK/x86_64/prefix/lib/libgnutls.30.dylib" \
        -output "$OUT/lib/libgnutls.30.dylib"
    install_name_tool -id "@rpath/libgnutls.30.dylib" "$OUT/lib/libgnutls.30.dylib"
    ln -sf libgnutls.30.dylib "$OUT/lib/libgnutls.dylib"
    echo "### gnutls OK -> $OUT/lib/libgnutls.30.dylib"
fi
# Wine reads GnuTLS's flags through pkg-config. nettle and hogweed are inside the library, so the .pc
# file names no further packages.
sed -e "s|$WORK/x86_64/prefix|$OUT|g" -e '/^Requires.private:/d' \
    "$WORK/x86_64/prefix/lib/pkgconfig/gnutls.pc" > "$OUT/lib/pkgconfig/gnutls.pc"

# wine-mono, which mscoree finds unpacked under the engine's share/wine/mono (stage-engine.sh): .NET
# programs run without Wine offering to download it. The version is the one dlls/mscoree expects.
MONO_VERSION="$(sed -n 's/^#define WINE_MONO_VERSION "\(.*\)"$/\1/p' "$WS_REPO/dlls/mscoree/mscoree_private.h")"
fetch "https://dl.winehq.org/wine/wine-mono/$MONO_VERSION/wine-mono-$MONO_VERSION-x86.tar.xz" >/dev/null
echo "### wine-mono $MONO_VERSION present"

# The media runtime winegstreamer and winedmo link: GLib, GStreamer and FFmpeg.
"$HERE/build_media.sh"

echo "### deps ready under $OUT"
