#!/bin/bash
# build_gnu.sh <name> <version> <url> [configure args...]
# Build one GNU-style host tool from a release tarball into toolchain/build (gitignored), with the
# in-tree clang, for the 10.9 host. WS_GNU_CC overrides the compiler for a tool whose source the
# in-tree clang rejects.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../../scripts/build-log.sh"
build_log_open
. "$HERE/../../scripts/env.sh"
NAME="$1" VERSION="$2" URL="$3"; shift 3
SRC="$WS_TOOLCHAIN/src"
mkdir -p "$SRC"
TARBALL="$SRC/$(basename "$URL")"
[ -f "$TARBALL" ] || curl -fsSL --retry 5 --connect-timeout 30 --speed-time 60 --speed-limit 1000 -o "$TARBALL.part" "$URL" && mv -f "$TARBALL.part" "$TARBALL" 2>/dev/null || true
WORK="$(mktemp -d "${TMPDIR:-/tmp}/$NAME-build.XXXXXX")"
trap 'rc=$?; rm -rf "$WORK"; build_log_report $rc' EXIT
echo "### building $NAME $VERSION"
tar xzf "$TARBALL" -C "$WORK"
cd "$WORK/$NAME-$VERSION"
if [ -n "${WS_GNU_CC:-}" ]; then
    TOOL_CC="$WS_GNU_CC -mmacosx-version-min=10.9" TOOL_LDFLAGS=""
else
    TOOL_CC="$WS_HOST_CC $(ws_host_cflags x86_64)" TOOL_LDFLAGS="$(ws_host_ldflags x86_64)"
fi
CC="$TOOL_CC" LDFLAGS="$TOOL_LDFLAGS" \
    ./configure --prefix="$WS_TOOLCHAIN" "$@"
make -j"$WS_JOBS"
make install
echo "### $NAME $VERSION OK"
