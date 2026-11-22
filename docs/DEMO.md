# Demo video — three minutes, shot by shot

Rules: under 3 minutes, public on YouTube, footage must show the project running
on the platform it was built for (the QNX 8.0 QEMU guest), no third-party
trademarks or music you do not have rights to. Judges need not watch past 3:00,
so the first 20 seconds must already show a deadline being enforced.

Record at 1080p, terminal font at least 16 pt, dark theme, and have the report
page open in a browser tab for the last shot.

## Before you press record

On the host:

```sh
. ~/qnx800/qnxsdp-env.sh
./qnx/build-qnx.sh
cd ~/qnx-vm && mkqnximage --run && mkqnximage --getip
cd - && ./qnx/deploy-to-qemu.sh <vm-ip>
```

On the guest, once, to check everything is in place:

```sh
cd /tmp && ./run-demo.sh --quick
```

Clear `/tmp/*.slg` afterwards so the recording starts clean.

## 0:00 – 0:20 | The problem, in one picture

**Screen**: the guest shell, idle.

**Voice-over**:
> "A control loop that misses its deadline doesn't slow down — it lies. The
> actuator is commanded with data older than the loop assumes. This is Sentinel:
> it enforces every job's deadline on QNX 8.0, and it shows you the miss before
> it happens."

## 0:20 – 0:50 | Healthy baseline

**Type** (show the commands, do not cut them):

```sh
cd /tmp
./sentineld -c ./sentinel.conf -l /tmp/sentinel.slg &
sleep 1
./plant-sim -n control -P 1 -B 0.9 -w 0.30 -r 20 -d 60 &
./loadgen -n 2 -w 400 -s 100 -d 60 &
sleep 2
./sentinel-ctl status
```

**Show**: `mode=NOMINAL`, `misses=0`, execution p99 a few hundred µs against a
900 µs budget. Say the numbers out loud: "one thousand jobs per second, a
0.9 millisecond budget, and two competing CPU hogs already running."

**Point out**: the supervisor is a resource manager — `ls /dev/sentinel`.

## 0:50 – 1:30 | An overrun, and the prediction before it

**Type**: `kill -USR1 %2` (the plant injects a 3 ms overrun, or wait for the
scripted fault at t=8 s).

**Show**: the supervisor console printing `MISS task=control seq=... exec=...`,
then `MODE NOMINAL -> WATCH (predicted overrun risk)` and `WATCH -> DEGRADED`,
and on the plant side `action -> THROTTLE`.

**Voice-over**:
> "Watch the order. The predictor saw the execution-time tail grow and cut the
> optional stage *before* the job actually missed — risk first, then the miss.
> The control loop learns the new mode in the same message round trip as its job
> start, so there is no window where it is still driving at full rate."

**Show**: `./sentinel-ctl status` — `misses=1`, `risk` percentage, worst slack
negative.

## 1:30 – 2:05 | The stall, and the safe stop

**Type**: `kill -USR2 %2` twice, 1 s apart (40 ms blocking stalls) — or wait for t=16, 17, 18 s.

**Show**: `MODE DEGRADED -> SAFE-STOP`, `action -> HOLD`, and the plant freezing
its setpoint. `./sentinel-ctl status` shows `SAFE-STOP (actuator held at safe
state, latched)`.

**Voice-over**:
> "Three critical misses inside the window and the supervisor latches SAFE-STOP.
> The actuator holds its last safe setpoint and stays there — it does not clear
> itself. Only an operator can bring it back."

**Type**: `./sentinel-ctl reset` → back to NOMINAL, then watch it degrade again
if the load is still there. This shows the ladder is not a one-way trap.

## 2:05 – 2:35 | The evidence

**Switch** to the host browser with `report.html`.

**Show**: the mode timeline (green → blue → amber → red), the per-task
histogram with the budget line, the transition table with reasons, the miss
counts, p99/p99.9.

**Voice-over**:
> "Every job, every miss and every mode change goes into a CRC-protected log,
> written by a low-priority thread so the real-time path never waits on a disk.
> `sentinel-report` is a host tool with no QNX dependency — it recomputed these
> percentiles from the raw records."

## 2:35 – 3:00 | Why QNX

**Show**: the code, `src/plat/plat_qnx.c`, scrolled to the timer setup and the
resmgr attach. Then a split with the README's evidence table.

**Voice-over**:
> "The enforcement path is eight microseconds of kernel timer and a pulse: no
> polling, no allocation, no lock. Sentinel is a resource manager, so tasks talk
> to it with plain open and devctl while the data stays a native message. That
> combination — a guaranteed priority relationship plus a kernel notification at
> the deadline — is exactly what a general-purpose kernel cannot promise, and it
> is why this belongs on QNX."

**End card**: repository URL, category, "QNX SDP 8.0, QEMU, no hardware".

## Recording notes

* Record the guest console and the host terminal separately, then cut. You do
  not have to show a screen recorder fighting for CPU while measuring jitter.
* If a number looks bad (host noise bleeding into the guest), do not hide it —
  say what it is. Honest numbers with an explanation read better than perfect
  numbers that contradict the log.
* Keep a copy of the `.slg` and the report of the exact run you record, so the
  video's claims can be reproduced.
