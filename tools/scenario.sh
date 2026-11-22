#!/bin/sh
# scenario.sh - end-to-end run of the whole system on the host.
#
# Starts the supervisor, a 1 kHz control loop and a competing load, injects
# two faults (an overrun and a blocking stall), then checks that the
# supervisor degraded and stopped the plant, and renders the report.
#
# Usage: ./tools/scenario.sh [seconds]
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build"
RUN="$ROOT/run"
DUR=${1:-12}
SOCK="$RUN/sentinel.sock"
LOG="$RUN/sentinel.slg"
mkdir -p "$RUN"
rm -f "$SOCK" "$LOG" "$RUN"/*.log "$RUN"/*.json "$RUN"/*.html

fail() {
    echo "SCENARIO FAILED: $*" >&2
    exit 1
}

"$BIN/sentineld" -c "$ROOT/etc/sentinel.conf" -p "$SOCK" -l "$LOG" \
    >"$RUN/daemon.log" 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null || true' EXIT INT TERM

i=0
while [ ! -S "$SOCK" ]; do
    i=$((i + 1))
    [ $i -gt 50 ] && fail "supervisor did not come up"
    sleep 0.1
done

# Competing CPU load for the whole run: this is what a real system looks
# like when navigation and vision are busy.
"$BIN/loadgen" -n 2 -w 300 -s 200 -d "$DUR" >"$RUN/loadgen.log" 2>&1 &
LPID=$!

# 1 kHz control loop, 0.9 ms budget, fault at 4 s (3 ms overrun) and at
# 7 s (40 ms blocking stall).
"$BIN/plant-sim" -p "$SOCK" -n control -P 1 -B 0.9 -w 0.30 -d "$DUR" \
    -f at=4:overrun:3:0 -f at=7:stall:40:0 >"$RUN/plant.log" 2>&1 &
PPID_=$!

sleep 2
echo "--- status at t=2 s (nominal) ---"
"$BIN/sentinel-ctl" -p "$SOCK" status

wait $PPID_ || true
echo "--- status at end of run ---"
"$BIN/sentinel-ctl" -p "$SOCK" status
"$BIN/sentinel-ctl" -p "$SOCK" status --json >"$RUN/status.json"

kill $LPID 2>/dev/null || true
wait $LPID 2>/dev/null || true
kill $DPID 2>/dev/null || true
sleep 0.3

"$BIN/sentinel-report" "$LOG" -t 0=control -j "$RUN/report.json" -h "$RUN/report.html"

echo
echo "--- plant ---"
cat "$RUN/plant.log"
echo "--- supervisor ---"
cat "$RUN/daemon.log"

# Assertions: the scenario is only useful if the safety path actually ran.
grep -q -- '-> WATCH\|-> DEGRADED' "$RUN/daemon.log" || fail "no escalation recorded"
grep -q -- '-> SAFE-STOP' "$RUN/daemon.log" || fail "no safe stop recorded"
grep -q 'action -> HOLD' "$RUN/plant.log" || fail "control loop never held"

echo
echo "SCENARIO PASSED (log $LOG, report $RUN/report.html)"
