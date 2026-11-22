#!/bin/sh
# run-demo.sh - run this ON THE QNX TARGET (the QEMU guest).
#
# Starts the supervisor, a 1 kHz control loop with a competing CPU load,
# injects an overrun and then a blocking stall, and prints the status view
# the monitor shows. Everything is timed so the whole story fits in a
# three-minute demo video.
#
#   ./run-demo.sh           12 s fault-free baseline, then faults
#   ./run-demo.sh --quick   short version for a smoke test
set -eu

DIR=$(dirname "$0")
cd "$DIR"
LOG=/tmp/sentinel.slg
SOCK=/dev/sentinel
DUR=${2:-34}
QUICK=0
[ "${1:-}" = "--quick" ] && { QUICK=1; DUR=12; }

rm -f "$LOG"

echo "== starting supervisor (priority 30) =="
./sentineld -c ./sentinel.conf -l "$LOG" &
DPID=$!
sleep 1

echo "== control loop: 1 kHz, 0.9 ms budget, priority 20 =="
./plant-sim -n control -P 1 -B 0.9 -w 0.30 -r 20 -d "$DUR" \
    -f at=8:overrun:3:0 -f at=16:stall:40:0 -f at=17:stall:40:0 \
    -f at=18:stall:40:0 -f at=24:burst:4:0.6 \
    &
PPID_=$!

echo "== competing load: 2 workers at 80% duty =="
./loadgen -n 2 -w 400 -s 100 -d "$DUR" &
LPID=$!

sleep 2
echo "== baseline (expect NOMINAL, no misses) =="
./sentinel-ctl status

sleep 6
echo "== after a 3 ms overrun (expect WATCH/DEGRADED) =="
./sentinel-ctl status

sleep 8
echo "== after a 40 ms blocking stall (expect SAFE-STOP) =="
./sentinel-ctl status

wait $PPID_ 2>/dev/null || true
echo "== final =="
./sentinel-ctl status
./sentinel-ctl status --json > /tmp/sentinel-status.json

kill $LPID $DPID 2>/dev/null || true
sleep 1
echo "== event log at $LOG =="
ls -l "$LOG"
echo
echo "copy it to the host and render the report there:"
echo "  scp root@\$(hostname):$LOG ./sentinel.slg"
echo "  ./build/sentinel-report sentinel.slg -t 0=control -j report.json -h report.html"
