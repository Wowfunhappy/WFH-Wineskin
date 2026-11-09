#!/bin/bash
# build_media.sh -- the media runtime winegstreamer and winedmo load, universal (i386 + x86_64), into
# deps/build: GLib, GStreamer with the plugins Wine's media pipeline uses, and FFmpeg under gst-libav.
#
#   GLib 2.80.5                 glib, gobject, gmodule, gio (libffi 3.4.6, pcre2 and proxy-libintl
#                               built in)
#   orc 0.4.41                  the SIMD code generator behind videoconvert, audioconvert and deinterlace
#   GStreamer 1.28.7            core (coreelements), and only these plugins:
#     plugins-base              app audioconvert audioresample pbtypes playback rawparse typefind
#                               videoconvertscale
#     plugins-good              apetag audioparsers avi deinterlace id3demux isomp4 matroska
#                               videofilter wavparse
#     plugins-bad               mpegdemux mpegtsdemux videoparsers
#     plugins-ugly              asfdemux (ASF, WMV, WMA)
#     gst-libav                 every FFmpeg decoder: H.264, MPEG-1/2/4, VC-1, WMV, WMA, AAC, MP3, ...
#   FFmpeg 8.1.3                libavcodec, libavformat, libavfilter, libavutil, libswresample,
#                               libswscale: decoders, demuxers and parsers only
#
# Each package is built once per architecture into deps/work/media/<arch>/prefix, against the
# system's own headers with deployment target 10.9 (scripts/env.sh), and the two prefixes are merged
# with lipo into deps/build. Every library's install name is @rpath/<name>; a library finds its
# siblings through @loader_path and a plugin in lib/gstreamer-1.0 through @loader_path/.., so the
# runtime works from wherever lib/ is copied. Idempotent: a package already installed in a prefix is
# skipped; deleting deps/work/media/<arch>/prefix rebuilds everything.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/build-log.sh"
build_log_open
. "$HERE/../scripts/env.sh"
MW="$HERE/work/media"
SRC="$MW/src"
OUT="$HERE/build"
mkdir -p "$SRC" "$OUT/lib" "$OUT/include"

GLIB_VER=2.80.5
GST_VER=1.28.7
ORC_VER=0.4.41
FFMPEG_VER=8.1.3

fetch() {  # <url> [file name]
    local t="$SRC/${2:-$(basename "$1")}"
    [ -f "$t" ] || { curl -fsSL --retry 5 --connect-timeout 30 --speed-time 60 --speed-limit 1000 -o "$t.part" "$1" && mv "$t.part" "$t"; }
    echo "$t"
}

# Meson and ninja, in a virtualenv of their own.
MESONENV="$MW/mesonenv"
if [ ! -x "$MESONENV/bin/meson" ]; then
    /usr/local/bin/python3 -m venv "$MESONENV"
    "$MESONENV/bin/pip" -q install "meson==1.5.2" ninja
fi
MESON="$MESONENV/bin/meson"
export PATH="$MESONENV/bin:$PATH"

GLIB_TARBALL="$(fetch https://download.gnome.org/sources/glib/${GLIB_VER%.*}/glib-$GLIB_VER.tar.xz)"
PCRE2_TARBALL="$(fetch https://github.com/PhilipHazel/pcre2/releases/download/pcre2-10.42/pcre2-10.42.tar.bz2)"
PCRE2_PATCH="$(fetch https://wrapdb.mesonbuild.com/v2/pcre2_10.42-2/get_patch pcre2_10.42-2_patch.zip)"
FFI_TARBALL="$(fetch https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz)"
INTL_TARBALL="$(fetch https://github.com/frida/proxy-libintl/archive/refs/tags/0.4.tar.gz proxy-libintl-0.4.tar.gz)"
ORC_TARBALL="$(fetch https://gstreamer.freedesktop.org/src/orc/orc-$ORC_VER.tar.xz)"
FFMPEG_TARBALL="$(fetch https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz)"
for m in gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly gst-libav; do
    fetch "https://gstreamer.freedesktop.org/src/$m/$m-$GST_VER.tar.xz" >/dev/null
done

# The host compiler, minus two things meson's links carry:
# - -Wl,-no_weak_imports, which 10.9's ld64 does not know (against 10.9's own headers there is no
#   newer function to import weakly);
# - a second -Wl,-rpath naming a path already named (meson names the prefix once per dependency):
#   meson's install deletes the build paths with install_name_tool, and 10.9's corrupts an image when
#   it deletes a path the image holds more than once.
mkdir -p "$MW/bin"
cat > "$MW/bin/cc" <<EOF
#!/bin/sh
seen=" "
for a; do
    shift
    case "\$a" in
        -Wl,-no_weak_imports) continue ;;
        -Wl,-rpath,*) case "\$seen" in *" \$a "*) continue ;; esac; seen="\$seen\$a " ;;
    esac
    set -- "\$@" "\$a"
done
exec "$WS_HOST_CC" "\$@"
EOF
chmod +x "$MW/bin/cc"

# The options every GStreamer module shares. auto_features=disabled builds only the plugins named.
GST_COMMON="-Dbuildtype=release -Ddefault_library=shared -Dauto_features=disabled -Dc_std=gnu11
    -Dtests=disabled -Ddoc=disabled -Dnls=disabled"

build_arch() {  # <i386|x86_64>
    local arch="$1" P="$MW/$1/prefix" B="$MW/$1/build"
    mkdir -p "$P/lib/pkgconfig" "$B"
    # The arch-specific compiler; meson reads the target CPU from its predefined macros.
    local cc="$MW/bin/cc $(ws_host_cflags "$arch")"
    export CC="$cc" OBJC="$cc" CXX="$MW/bin/cc --driver-mode=g++ $(ws_host_cflags "$arch")"
    export CFLAGS="-O2" OBJCFLAGS="-O2" CPPFLAGS="-I$P/include"
    export LDFLAGS="-L$P/lib -Wl,-headerpad_max_install_names"
    export PKG_CONFIG="$WS_TOOLCHAIN/bin/pkg-config"
    export PKG_CONFIG_PATH="$P/lib/pkgconfig" PKG_CONFIG_LIBDIR="$P/lib/pkgconfig"

    meson_pkg() {  # <name> <src dir> <stamp file> [meson options...]
        local name="$1" dir="$2" stamp="$3"; shift 3
        if [ -e "$P/$stamp" ]; then echo "### $name ($arch): present"; return; fi
        echo "### $name ($arch)"
        rm -rf "$dir/_build"
        ( cd "$dir" && "$MESON" setup _build --prefix="$P" --libdir=lib "$@" \
            && "$MESON" compile -C _build -j "$WS_JOBS" && "$MESON" install -C _build ) || exit 1
    }
    unpack() {  # <tarball>: extract into the arch's build dir, echo the source dir
        local name; name="$(basename "$1")"; name="${name%.tar.*}"
        rm -rf "$B/$name"
        tar xf "$1" -C "$B"
        echo "$B/$name"
    }

    local d
    # libffi, static: 10.9's predates the closure API GLib calls.
    if [ ! -e "$P/lib/libffi.a" ]; then
        echo "### libffi ($arch)"
        d="$(unpack "$FFI_TARBALL")"
        ( cd "$d" && ./configure --prefix="$P" --host=$arch-apple-darwin --build=x86_64-apple-darwin \
            --disable-shared --enable-static --with-pic --disable-docs --disable-multi-os-directory \
            && make -j"$WS_JOBS" && make install ) || exit 1
    fi
    if [ ! -e "$P/lib/libglib-2.0.0.dylib" ]; then
        d="$(unpack "$GLIB_TARBALL")"
        mkdir -p "$d/subprojects/packagecache"
        cp "$PCRE2_TARBALL" "$PCRE2_PATCH" "$d/subprojects/packagecache/"
        tar xzf "$INTL_TARBALL" -C "$d/subprojects" && mv "$d/subprojects/proxy-libintl-0.4" "$d/subprojects/proxy-libintl"
        meson_pkg glib "$d" lib/libglib-2.0.0.dylib --force-fallback-for=libpcre2-8 \
            -Dbuildtype=release -Ddefault_library=shared -Dpcre2:default_library=static \
            -Dproxy-libintl:default_library=static -Dtests=false -Dinstalled_tests=false \
            -Dglib_debug=disabled -Dman-pages=disabled -Ddocumentation=false -Dintrospection=disabled \
            -Dnls=disabled -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dsysprof=disabled \
            -Ddtrace=false -Dsystemtap=false -Dlibelf=disabled
    fi
    if [ ! -e "$P/lib/liborc-0.4.0.dylib" ]; then
        d="$(unpack "$ORC_TARBALL")"
        meson_pkg orc "$d" lib/liborc-0.4.0.dylib -Dbuildtype=release -Dorc-test=disabled \
            -Dtests=disabled -Dexamples=disabled -Dbenchmarks=disabled -Dgtk_doc=disabled -Dtools=disabled
    fi
    # FFmpeg: decoders, demuxers, parsers and bitstream filters, no Apple framework. 10.9's assert.h
    # has no C11 static_assert, which FFmpeg's sources use. On i386, where size_t is unsigned long and
    # ptrdiff_t int, FFmpeg passes one for the other: the same width, which clang 22 treats as an error.
    if [ ! -e "$P/lib/libavcodec.dylib" ]; then
        echo "### FFmpeg ($arch)"
        d="$(unpack "$FFMPEG_TARBALL")"
        local ffarch=x86_64; [ "$arch" = i386 ] && ffarch=x86_32
        ( cd "$d" && ./configure --prefix="$P" --cc="$cc" --arch=$ffarch --target-os=darwin \
            --enable-shared --disable-static --install-name-dir='@rpath' \
            --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-network \
            --disable-encoders --disable-muxers --disable-devices --disable-filters --disable-hwaccels \
            --disable-audiotoolbox --disable-videotoolbox --disable-securetransport --disable-coreimage \
            --disable-avfoundation --disable-appkit --disable-metal --disable-iconv --disable-lzma \
            --disable-sdl2 --disable-xlib --disable-libxcb \
            --x86asmexe="$WS_TOOLCHAIN/bin/nasm" \
            --extra-cflags="-Dstatic_assert=_Static_assert -Wno-error=incompatible-pointer-types" \
            --extra-ldflags="-Wl,-headerpad_max_install_names" \
            && make -j"$WS_JOBS" && make install ) || exit 1
    fi
    if [ ! -e "$P/lib/libgstreamer-1.0.0.dylib" ]; then
        d="$(unpack "$SRC/gstreamer-$GST_VER.tar.xz")"
        # gst_macos_main(), which the tools run: AppKit constants by their 10.9 names, and properties
        # synthesized as i386's Objective-C runtime requires.
        ( cd "$d" && patch -p1 < "$HERE/patches/gstreamer-macos-10.9.patch" ) || exit 1
        meson_pkg gstreamer "$d" lib/libgstreamer-1.0.0.dylib $GST_COMMON -Dintrospection=disabled \
            -Dtools=enabled -Dgst_parse=true -Dregistry=true -Dbenchmarks=disabled -Dexamples=disabled \
            -Dlibunwind=disabled -Dlibdw=disabled -Ddbghelp=disabled -Dbash-completion=disabled \
            -Dcheck=disabled -Dptp-helper=disabled -Dcoretracers=disabled
    fi
    if [ ! -e "$P/lib/gstreamer-1.0/libgstplayback.dylib" ]; then
        d="$(unpack "$SRC/gst-plugins-base-$GST_VER.tar.xz")"
        meson_pkg gst-plugins-base "$d" lib/gstreamer-1.0/libgstplayback.dylib $GST_COMMON \
            -Dintrospection=disabled -Dorc=enabled -Dtools=disabled \
            -Dapp=enabled -Daudioconvert=enabled -Daudioresample=enabled -Dpbtypes=enabled \
            -Dplayback=enabled -Drawparse=enabled -Dtypefind=enabled -Dvideoconvertscale=enabled
    fi
    if [ ! -e "$P/lib/gstreamer-1.0/libgstisomp4.dylib" ]; then
        d="$(unpack "$SRC/gst-plugins-good-$GST_VER.tar.xz")"
        meson_pkg gst-plugins-good "$d" lib/gstreamer-1.0/libgstisomp4.dylib $GST_COMMON -Dorc=enabled \
            -Dapetag=enabled -Daudioparsers=enabled -Davi=enabled -Ddeinterlace=enabled \
            -Did3demux=enabled -Disomp4=enabled -Dmatroska=enabled -Dvideofilter=enabled -Dwavparse=enabled
    fi
    if [ ! -e "$P/lib/gstreamer-1.0/libgstvideoparsersbad.dylib" ]; then
        d="$(unpack "$SRC/gst-plugins-bad-$GST_VER.tar.xz")"
        meson_pkg gst-plugins-bad "$d" lib/gstreamer-1.0/libgstvideoparsersbad.dylib $GST_COMMON \
            -Dintrospection=disabled -Dorc=enabled -Dtools=disabled \
            -Dmpegdemux=enabled -Dmpegtsdemux=enabled -Dvideoparsers=enabled
    fi
    if [ ! -e "$P/lib/gstreamer-1.0/libgstasf.dylib" ]; then
        d="$(unpack "$SRC/gst-plugins-ugly-$GST_VER.tar.xz")"
        meson_pkg gst-plugins-ugly "$d" lib/gstreamer-1.0/libgstasf.dylib $GST_COMMON -Dorc=enabled \
            -Dasfdemux=enabled
    fi
    if [ ! -e "$P/lib/gstreamer-1.0/libgstlibav.dylib" ]; then
        d="$(unpack "$SRC/gst-libav-$GST_VER.tar.xz")"
        meson_pkg gst-libav "$d" lib/gstreamer-1.0/libgstlibav.dylib -Dbuildtype=release \
            -Ddefault_library=shared -Dtests=disabled -Ddoc=disabled
    fi
}

for arch in i386 x86_64; do
    ( build_arch "$arch" ) || { echo "### media runtime: $arch FAILED" >&2; exit 1; }
done

echo "### media runtime: merge into $OUT"
P32="$MW/i386/prefix" P64="$MW/x86_64/prefix"
TMP="$MW/merge"
# fix <thin image> <rpath>: the image's install name and every reference into a prefix become
# @rpath/<leaf>, and <rpath> its one rpath. meson installs with absolute install names; FFmpeg's are
# @rpath already.
fix() {
    local o="$1" r
    r="$(otool -D "$o" | sed -n "2s|^$MW/.*/||p")"
    [ -z "$r" ] || install_name_tool -id "@rpath/$r" "$o" || return 1
    for r in $(otool -L "$o" | awk -v mw="$MW/" 'index($1, mw) == 1 { print $1 }'); do
        install_name_tool -change "$r" "@rpath/${r##*/}" "$o" || return 1
    done
    # A meson-built image keeps the prefix as its rpath.
    r="$(otool -l "$o" | awk '$1 == "cmd" { c = $2 } c == "LC_RPATH" && $1 == "path" { print $2 }' | sort -u)"
    case "$r" in
        "") install_name_tool -add_rpath "$2" "$o" ;;
        "$MW/"*/prefix/lib) install_name_tool -rpath "$r" "$2" "$o" ;;
        *) echo "### $o: rpaths $r" >&2; return 1 ;;
    esac
}
merge() {  # <path in a prefix> <rpath>: the two architectures' image, fixed, as one in deps/build
    local a
    for a in i386 x86_64; do
        rm -f "$TMP/$a"
        cp "$MW/$a/prefix/$1" "$TMP/$a" && chmod u+w "$TMP/$a" && fix "$TMP/$a" "$2" || return 1
    done
    mkdir -p "$(dirname "$OUT/$1")"
    rm -f "$OUT/$1"
    lipo -create "$TMP/i386" "$TMP/x86_64" -output "$OUT/$1"
}
deps_of() {  # <image>: the leaves of its @rpath references, but its own name
    otool -L "$1" | awk -v self="@rpath/${1##*/}" \
        'index($1, "@rpath/") == 1 && $1 != self { sub(/^@rpath\//, "", $1); print $1 }'
}
rm -rf "$TMP" "$OUT/lib/gstreamer-1.0" "$OUT/libexec/gstreamer-1.0"
mkdir -p "$TMP"
for f in $(cd "$P64/lib" && ls *.dylib); do rm -f "$OUT/lib/$f"; done

# GStreamer's plugins, the plugin scanner and the command-line tools (which find the libraries in
# ../lib), and the libraries Wine links (pkg-config's -l names, by their install names' leaves).
for f in $(cd "$P64" && ls lib/gstreamer-1.0/*.dylib); do merge "$f" @loader_path/.. || exit 1; done
for f in libexec/gstreamer-1.0/gst-plugin-scanner bin/gst-inspect-1.0 bin/gst-launch-1.0; do
    merge "$f" "@loader_path/$(dirname "$f" | sed 's|[^/]*|..|g')/lib" || exit 1
done
WINE_PC="gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0 libavutil libavformat libavcodec"
: > "$TMP/libs"
for l in $(PKG_CONFIG_LIBDIR="$P64/lib/pkgconfig" "$WS_TOOLCHAIN/bin/pkg-config" --libs-only-l $WINE_PC); do
    [ "$l" != -lintl ] || continue
    leaf="$(otool -D "$P64/lib/lib${l#-l}.dylib" | sed -n '2s|.*/||p')"
    ln -sfn "$leaf" "$OUT/lib/lib${l#-l}.dylib"
    echo "$leaf" >> "$TMP/libs"
done
# Every library those reach: lib/media-runtime.txt names them by leaf, the name the engine needs.
{ deps_of "$OUT/libexec/gstreamer-1.0/gst-plugin-scanner"
  for f in "$OUT"/lib/gstreamer-1.0/*.dylib; do deps_of "$f"; done; } >> "$TMP/libs"
sort -u "$TMP/libs" > "$TMP/todo"
: > "$TMP/done"
while [ -s "$TMP/todo" ]; do
    for leaf in $(cat "$TMP/todo"); do
        merge "lib/$leaf" @loader_path || exit 1
        echo "$leaf" >> "$TMP/done"
        deps_of "$OUT/lib/$leaf" >> "$TMP/libs"
    done
    sort -u "$TMP/done" > "$TMP/done.sorted"
    sort -u "$TMP/libs" | comm -23 - "$TMP/done.sorted" > "$TMP/todo"
done
mv "$TMP/done.sorted" "$OUT/lib/media-runtime.txt"
rm -rf "$TMP"
# Every reference outside the system is @rpath, so the merged runtime holds no build path.
if otool -L "$OUT"/lib/gstreamer-1.0/*.dylib "$OUT"/libexec/gstreamer-1.0/* "$OUT"/bin/gst-* \
        $(sed "s|^|$OUT/lib/|" "$OUT/lib/media-runtime.txt") \
        | grep -v ':$' | grep -v -e '@rpath/' -e '/usr/lib/' -e '/System/Library/'; then
    echo "### media runtime: a library refers to a build path" >&2; exit 1
fi

# Headers and pkg-config files. The headers are the x86_64 install's; a header that differs between
# the architectures is merged under __LP64__.
for h in $(cd "$P64" && find include lib/glib-2.0/include -name '*.h'); do
    mkdir -p "$(dirname "$OUT/$h")"
    if cmp -s "$P32/$h" "$P64/$h"; then
        cp "$P64/$h" "$OUT/$h"
    else
        { echo "#ifdef __LP64__"; cat "$P64/$h"; echo "#else"; cat "$P32/$h"; echo "#endif"; } > "$OUT/$h"
    fi
done
# The .pc files name no rpath (a module that links the runtime names its own) and no libintl, which
# is inside libglib.
mkdir -p "$OUT/lib/pkgconfig"
for pc in "$P64"/lib/pkgconfig/*.pc; do
    sed -e "s|$P64|$OUT|g" -e 's| -Wl,-rpath,[^ ]*||g' -e 's| -lintl||g' "$pc" > "$OUT/lib/pkgconfig/$(basename "$pc")"
done
echo "### media runtime ready under $OUT"
