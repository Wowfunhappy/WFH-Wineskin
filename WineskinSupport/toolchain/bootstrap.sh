#!/bin/bash
# Reconstruct the toolchain ARTIFACTS (toolchain/build/) from the COMMITTED inputs (toolchain/vendor/
# and toolchain/scripts/). Idempotent; delete toolchain/build/ to force a full rebuild.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$HERE/vendor"
BUILD="$HERE/build"
SCRIPTS="$HERE/scripts"
mkdir -p "$BUILD/bin"

echo "### [1/8] assemble build/clang from vendor/clang"
CLANG_OUT="$BUILD/clang"
if [ ! -x "$CLANG_OUT/bin/clang-22" ]; then
    rm -rf "$CLANG_OUT"; mkdir -p "$CLANG_OUT/bin"
    bunzip2 -c "$VENDOR/clang/bin/clang-22.bz2" > "$CLANG_OUT/bin/clang-22"
    bunzip2 -c "$VENDOR/clang/bin/lld.bz2"      > "$CLANG_OUT/bin/lld"
    chmod +x "$CLANG_OUT/bin/clang-22" "$CLANG_OUT/bin/lld"
    cp "$VENDOR/clang/bin/llvm-ar" "$CLANG_OUT/bin/"
    # Multi-call binaries: the names they answer to. Wine's PE side links with lld-link and builds
    # import libraries with llvm-dlltool.
    ( cd "$CLANG_OUT/bin"
      ln -sf clang-22 clang; ln -sf clang clang++; ln -sf clang clang-cpp
      ln -sf lld ld64.lld; ln -sf lld lld-link; ln -sf lld ld.lld
      ln -sf llvm-ar llvm-ranlib; ln -sf llvm-ar llvm-dlltool; ln -sf llvm-ar llvm-lib )
    cp -R "$VENDOR/clang/lib" "$CLANG_OUT/lib"
    ( cd "$CLANG_OUT/lib"
      for base in libc++ libc++abi libunwind; do
        ln -sf "$base.1.0.dylib" "$base.1.dylib"; ln -sf "$base.1.dylib" "$base.dylib"
      done )
    echo "    clang $("$CLANG_OUT/bin/clang" --version | head -1 | awk '{print $3}') assembled"
else
    echo "    already present, skipping"
fi

# The host compiler: the in-tree clang with ld64 as its Mach-O linker, as Apple's clang has it. lld's
# Mach-O port has no i386 support and no segment placement options, which the Wine loader and preloader
# need. The linker options go only on commands that link.
mkdir -p "$BUILD/host/bin"
cat > "$BUILD/host/bin/clang" <<'WRAPPER'
#!/bin/sh
for arg do
    case "$arg" in
        -c|-S|-E|-M|-MM|-fsyntax-only) exec "$(dirname "$0")/../../clang/bin/clang" "$@" ;;
    esac
done
exec "$(dirname "$0")/../../clang/bin/clang" "$@" -fuse-ld=/usr/bin/ld -mlinker-version=241.9
WRAPPER
chmod +x "$BUILD/host/bin/clang"

GNU=https://ftp.gnu.org/gnu
echo "### [2/8] m4";       [ -x "$BUILD/bin/m4" ]       || "$SCRIPTS/build_gnu.sh" m4 1.4.19 "$GNU/m4/m4-1.4.19.tar.gz" --disable-nls
echo "### [3/8] autoconf"; [ "$("$BUILD/bin/autoconf" --version 2>/dev/null | head -1)" = "autoconf (GNU Autoconf) 2.73" ] || "$SCRIPTS/build_gnu.sh" autoconf 2.73 "$GNU/autoconf/autoconf-2.73.tar.gz" --disable-nls
echo "### [4/8] bison";    [ -x "$BUILD/bin/bison" ]    || "$SCRIPTS/build_gnu.sh" bison 3.8.2 "$GNU/bison/bison-3.8.2.tar.gz" --disable-nls
echo "### [5/8] make";     [ -x "$BUILD/bin/make" ]     || "$SCRIPTS/build_gnu.sh" make 4.4.1 "$GNU/make/make-4.4.1.tar.gz" --disable-nls
echo "### [6/8] pkgconf"
if [ ! -x "$BUILD/bin/pkg-config" ]; then
    "$SCRIPTS/build_gnu.sh" pkgconf 2.3.0 https://distfiles.ariadne.space/pkgconf/pkgconf-2.3.0.tar.gz --disable-nls
    ln -sf pkgconf "$BUILD/bin/pkg-config"
fi

echo "### [7/8] nasm";     [ -x "$BUILD/bin/nasm" ]     || WS_GNU_CC=/usr/bin/cc "$SCRIPTS/build_gnu.sh" nasm 2.16.03 \
    https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.gz

# ccache 3.7 is the last release built with autotools rather than CMake.
echo "### [8/8] ccache";   [ -x "$BUILD/bin/ccache" ]   || "$SCRIPTS/build_gnu.sh" ccache 3.7.12 \
    https://github.com/ccache/ccache/releases/download/v3.7.12/ccache-3.7.12.tar.gz

echo "### toolchain ready under $BUILD"
