#!/bin/sh
# test.sh - build and run the unit tests, then the end-to-end scenario.
#
#   ./tools/test.sh          unit tests only
#   ./tools/test.sh all      unit tests + IPC scenario (about 15 s)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build"
CC=${CC:-}

if [ -z "$CC" ]; then
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC=$c; break; fi
    done
fi
if [ -z "$CC" ] && command -v zig >/dev/null 2>&1; then CC="zig cc"; fi
if [ -z "$CC" ] && [ -n "${ZIG:-}" ]; then CC="$ZIG cc"; fi
if [ -z "$CC" ]; then
    echo "error: no C compiler found (set CC=...)" >&2
    exit 1
fi

CFLAGS=${CFLAGS:-"-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE"}
INCS="-I$ROOT/include"
LIB="$ROOT/src/lib/sl_clock.c $ROOT/src/lib/sl_stats.c $ROOT/src/lib/sl_sup.c $ROOT/src/lib/sl_log.c $ROOT/src/lib/sl_config.c"

"$ROOT/tools/build-host.sh" >/dev/null

mkdir -p "$BUILD"
fail=0
for t in test_stats test_sup test_log test_config; do
    # shellcheck disable=SC2086
    $CC $CFLAGS $INCS "$ROOT/tests/$t.c" $LIB -o "$BUILD/$t" -lpthread -lm
    if "$BUILD/$t"; then :; else fail=1; fi
done

if [ "${1:-}" = "all" ]; then
    if "$ROOT/tools/scenario.sh" 12 >"$ROOT/build/scenario.out" 2>&1; then
        echo "scenario: ok"
    else
        echo "scenario: FAILED (see build/scenario.out)"
        tail -30 "$ROOT/build/scenario.out"
        fail=1
    fi
fi

if [ $fail -ne 0 ]; then
    echo "TESTS FAILED"
    exit 1
fi
echo "all tests passed"
