#!/bin/bash
# env.sh -- SOURCED. The build environment every script here shares: the in-tree toolchain first on
# PATH, the 10.9 SDK, and the 10.9 deployment target.

WS_SUPPORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS_REPO="$(cd "$WS_SUPPORT/.." && pwd)"
WS_TOOLCHAIN="$WS_SUPPORT/toolchain/build"
WS_CLANG="$WS_TOOLCHAIN/clang/bin/clang"
# The host (Mach-O) compiler: WS_CLANG linking with ld64 (toolchain/bootstrap.sh).
WS_HOST_CC="$WS_TOOLCHAIN/host/bin/clang"
WS_DEPS="$WS_SUPPORT/deps/build"
WS_SDK="${WINESKIN_SDK:-/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.9.sdk}"
# The install root Wine is configured for; staging installs under it with DESTDIR.
WS_APP_ROOT=/Wineskin.app
WS_JOBS="${WS_JOBS:-$(sysctl -n hw.ncpu)}"

if [ ! -d "$WS_SDK" ]; then
    echo "env.sh: FATAL: no macOS 10.9 SDK at $WS_SDK (set WINESKIN_SDK)." >&2
    exit 1
fi

export MACOSX_DEPLOYMENT_TARGET=10.9

# Every Wine compile, host and PE, goes through ccache: a reconfigure that rewrites config.h touches
# nearly every object of both trees.
WS_CCACHE="$WS_TOOLCHAIN/bin/ccache"
export CCACHE_DIR="$WS_REPO/build/ccache"
export CCACHE_BASEDIR="$WS_REPO"
export CCACHE_MAXSIZE=30G
# The host compiler is a wrapper script; the compiler that matters is the clang binary behind it.
export CCACHE_COMPILERCHECK="string:$(/usr/bin/stat -f '%m-%z' "$WS_TOOLCHAIN/clang/bin/clang-22" 2>/dev/null)"
export PATH="$WS_TOOLCHAIN/bin:$WS_TOOLCHAIN/clang/bin:/usr/bin:/bin:/usr/sbin:/sbin"
unset SDKROOT CPATH LIBRARY_PATH C_INCLUDE_PATH OBJC_INCLUDE_PATH

# The Mach-O compiler driver flags for one host architecture, for WS_HOST_CC.
ws_host_cflags() {   # <i386|x86_64>
    echo "--no-default-config -arch $1 -isysroot $WS_SDK -mmacosx-version-min=10.9"
}
ws_host_ldflags() {  # <i386|x86_64>
    ws_host_cflags "$1"
}
