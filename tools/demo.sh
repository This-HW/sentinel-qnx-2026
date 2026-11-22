#!/bin/sh
# demo.sh - the host-simulation demo, shaped like the video story.
#
# Phase 1 (0-4 s)   healthy control loop with competing load
# Phase 2 (4 s)     a compute overrun       -> predicted risk, then a miss
# Phase 3 (9 s)     a 40 ms blocking stall  -> SAFE-STOP, actuator held
# Phase 4 (14 s)    operator reset          -> recovery to NOMINAL
#
# The loop runs at 100 Hz / 9 ms budget here, not the 1 kHz / 0.9 ms used on
# the target: a general-purpose kernel with an unprivileged process cannot keep
# a 1 ms loop, so a 1 kHz run on a laptop shows host noise, not the supervisor.
# Use qnx/run-demo.sh on the target for the real-time configuration.
#
# Artifacts land in demo/: report.html, report.json, transcript.log
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build"
OUT="$ROOT/demo"
SOCK="$OUT/sentinel.sock"
LOG="$OUT/sentinel.slg"

mkdir -p "$OUT"
rm -f "$SOCK" "$LOG" "$OUT"/*.json "$OUT"/*.html "$OUT"/*.log

"$BIN/sentineld" -c "$ROOT/etc/sentinel.conf" -p "$SOCK" -l "$LOG" \
    >"$OUT/supervisor.log" 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null || true' EXIT INT TERM

i=0
while [ ! -S "$SOCK" ]; do
    i=$((i + 1))
    [ $i -gt 50 ] && { echo "supervisor did not start" >&2; exit 1; }
    sleep 0.1
done

"$BIN/loadgen" -n 2 -w 350 -s 150 -d 20 >"$OUT/loadgen.log" 2>&1 &
PERIOD_MS=${PERIOD_MS:-10}
BUDGET_MS=${BUDGET_MS:-9}
WORK_MS=${WORK_MS:-1}
"$BIN/plant-sim" -p "$SOCK" -n control -P "$PERIOD_MS" -B "$BUDGET_MS" \
    -w "$WORK_MS" -d 20 -f at=4:overrun:14:0 -f at=9:stall:40:0 \
    -f at=10:stall:40:0 -f at=11:stall:40:0 \
    >"$OUT/plant.log" 2>&1 &
PPID_=$!

sleep 3
echo "=== t=3s  healthy ==="
"$BIN/sentinel-ctl" -p "$SOCK" status

sleep 3
echo "=== t=6s  after the overrun ==="
"$BIN/sentinel-ctl" -p "$SOCK" status

sleep 4
echo "=== t=10s after the stall ==="
"$BIN/sentinel-ctl" -p "$SOCK" status

wait $PPID_ || true

echo "=== operator reset ==="
"$BIN/sentinel-ctl" -p "$SOCK" reset
sleep 1
"$BIN/sentinel-ctl" -p "$SOCK" status
"$BIN/sentinel-ctl" -p "$SOCK" status --json >"$OUT/status.json"

kill $DPID 2>/dev/null || true
sleep 0.4

"$BIN/sentinel-report" "$LOG" -t 0=control -j "$OUT/report.json" -h "$OUT/report.html"
{
    echo "--- supervisor ---"
    cat "$OUT/supervisor.log"
    echo "--- plant ---"
    cat "$OUT/plant.log"
} | tee "$OUT/transcript.log"

echo
echo "artifacts: $OUT/report.html  $OUT/report.json  $OUT/transcript.log"
