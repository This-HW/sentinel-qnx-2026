#!/bin/sh
# build-host.sh - build and test Sentinel on a POSIX host (Linux/macOS).
#
# This is the development build: it compiles the portable core plus the
# POSIX backend, so the supervisor, the log format and every tool can be
# exercised without the QNX toolchain. The QNX build is qnx/build-qnx.sh.
#
# Usage:
#   ./tools/build-host.sh            build everything into build/
#   ./tools/build-host.sh clean      remove build/
#
# Honours $CC. Without one it looks for cc/gcc/clang, then `zig cc`
# (zig is a convenient self-contained C compiler for demo machines).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build"
CC=${CC:-}

if [ "$#" -gt 0 ] && [ "$1" = "clean" ]; then
    rm -rf "$BUILD"
    echo "cleaned $BUILD"
    exit 0
fi

if [ -z "$CC" ]; then
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC=$c; break; fi
    done
fi
if [ -z "$CC" ]; then
    if command -v zig >/dev/null 2>&1; then
        CC="zig cc"
    elif [ -n "${ZIG:-}" ]; then
        CC="$ZIG cc"
    else
        echo "error: no C compiler found (set CC=...)" >&2
        exit 1
    fi
fi

CFLAGS=${CFLAGS:-"-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE"}
INCS="-I$ROOT/include"
LIBS="-lpthread -lm"
LIB_SRC="$ROOT/src/lib/sl_clock.c $ROOT/src/lib/sl_stats.c $ROOT/src/lib/sl_sup.c $ROOT/src/lib/sl_log.c $ROOT/src/lib/sl_config.c"
PLAT_SRC="$ROOT/src/plat/plat_posix.c"

mkdir -p "$BUILD"

echo "build-host: compiler = $CC"

compile() {
    out=$1
    shift
    # shellcheck disable=SC2086
    $CC $CFLAGS $INCS "$@" -o "$BUILD/$out" $LIBS
    echo "  built build/$out"
}

compile sentineld $LIB_SRC $PLAT_SRC "$ROOT/src/sentineld/main.c"
compile plant-sim $LIB_SRC $PLAT_SRC "$ROOT/src/plant/plant_sim.c"
compile loadgen $LIB_SRC $PLAT_SRC "$ROOT/src/loadgen/loadgen.c"
compile sentinel-ctl $LIB_SRC $PLAT_SRC "$ROOT/src/ctl/sentinel_ctl.c"
compile sentinel-report $LIB_SRC "$ROOT/src/report/sentinel_report.c"

echo "build-host: ok"
