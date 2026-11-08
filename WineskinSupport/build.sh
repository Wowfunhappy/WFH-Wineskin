#!/bin/bash
# build.sh [64|32|all] -- configure (first run) and build the Wine trees under build/, then stage the
# combined WoW64 installation into build/engine. Logs to /tmp/wineskin_build.log.
# The polyfill layer is brought up to date first, and each tree tracks it (scripts/track-polyfill.sh).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/scripts/build-log.sh"
build_log_open
. "$HERE/scripts/env.sh"
WHICH="${1:-all}"

build_tree() {  # <64|32>
    local tree="$WS_REPO/build/wine$1"
    if [ ! -f "$tree/Makefile" ]; then
        echo "### configure wine$1"
        "$HERE/scripts/configure-wine.sh" "$1" || return $?
    fi
    "$HERE/scripts/track-polyfill.sh" "$1" || return $?
    echo "### build wine$1"
    make -C "$tree" -j"$WS_JOBS" -O || return $?
    echo "### wine$1 BUILT"
}

rc=0
"$HERE/polyfill/build-polyfill.sh" || rc=$?
[ $rc -ne 0 ] || case "$WHICH" in
    64) build_tree 64 || rc=$? ;;
    32) build_tree 32 || rc=$? ;;
    all) { build_tree 64 && build_tree 32 && "$HERE/scripts/stage-engine.sh"; } || rc=$? ;;
esac
echo "REBUILD DONE (rc=$rc)"
exit $rc
