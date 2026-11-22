#!/bin/sh
# build-qnx.sh - cross-build Sentinel for QNX 8.0 with the QNX SDP toolchain.
#
#   1. Install the free QNX SDP 8.0 (QNX Everywhere, non-commercial)
#   2. . ~/qnx800/qnxsdp-env.sh
#   3. ./qnx/build-qnx.sh
#
# Output goes to build-qnx/. QNX_VARIANT selects the toolchain:
#   gcc_ntox86_64    default, matches the QEMU x86_64 image
#   gcc_ntoaarch64le for a Raspberry Pi 5 or another AArch64 board
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/build-qnx"
VARIANT=${QNX_VARIANT:-gcc_ntox86_64}

if [ -z "${QNX_TARGET:-}" ]; then
    echo "error: QNX environment is not set." >&2
    echo "       run:  . ~/qnx800/qnxsdp-env.sh" >&2
    exit 1
fi
if ! command -v qcc >/dev/null 2>&1; then
    echo "error: qcc not found in PATH (source qnxsdp-env.sh first)" >&2
    exit 1
fi

CFLAGS=${CFLAGS:-"-O2 -g -Wall -Wextra -Wno-unused-parameter"}
LIB="$ROOT/src/lib/sl_clock.c $ROOT/src/lib/sl_stats.c $ROOT/src/lib/sl_sup.c \
     $ROOT/src/lib/sl_log.c $ROOT/src/lib/sl_config.c"
PLAT="$ROOT/src/plat/plat_qnx.c"

mkdir -p "$OUT"
echo "build-qnx: $(qcc -V$VARIANT 2>&1 | head -1)"

build() {
    out=$1
    shift
    # shellcheck disable=SC2086
    qcc -V$VARIANT $CFLAGS -I"$ROOT/include" "$@" -o "$OUT/$out" -lm
    echo "  built build-qnx/$out"
}

build sentineld      $LIB $PLAT "$ROOT/src/sentineld/main.c"
build plant-sim      $LIB $PLAT "$ROOT/src/plant/plant_sim.c"
build loadgen        $LIB $PLAT "$ROOT/src/loadgen/loadgen.c"
build sentinel-ctl   $LIB $PLAT "$ROOT/src/ctl/sentinel_ctl.c"
# The report tool is host-only; build it on the machine that holds the log
# (it has no QNX dependency) or copy the log to the host.

echo "build-qnx: ok -> $OUT"
echo "next: scp $OUT/sentineld $OUT/plant-sim $OUT/loadgen $OUT/sentinel-ctl \\"
echo "          etc/sentinel.conf root@<vm-ip>:/tmp/"
