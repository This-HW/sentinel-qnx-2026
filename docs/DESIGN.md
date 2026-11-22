# Design

## 1. The problem

A control loop that misses a deadline does not slow down, it *lies*: the
actuator is commanded with data that is older than the loop assumes. In a robot
joint that is a jerk, in an infusion pump it is a dose error, in a vehicle it is
a control input applied at the wrong time. The failure is not the latency, it is
that nobody knows the loop is no longer real-time until something else breaks.

Three things are therefore needed, and they are different problems:

1. **Detection.** Prove, per job, whether the deadline was met. Not "the CPU
   looked busy" — the job either finished inside its budget or it did not.
2. **Attribution.** When it fails, say which task, which job, and by how much,
   with evidence that survives a reboot.
3. **Prevention.** Move the system into a mode where the deadline *is* met
   before the deadline is actually missed, and if that is impossible, fail safe.

## 2. Why the enforcement path must be on the RTOS side

Sentinel does not measure deadlines by having the task tell it "I am late". A
task that is late is exactly the task that cannot be trusted to report it: it
may be blocked in a driver, spinning in a loop, or preempted by something
unknowable. So the supervisor holds a **kernel timer per running job**:

```
job start ──▶ supervisor arms timer for start + budget
                    │
        job done ───┴──▶ disarm, record exec/slack, feed the statistics
                    │
        timer fires ────▶ the job missed, no matter what the task says
```

The timer is a QNX POSIX timer created with `SIGEV_PULSE_INT_INIT`, delivering a
**pulse** to the resource manager's channel. The supervisor is therefore woken
by the kernel at the deadline, at its own priority, with no polling and no
cooperating thread. This is the part that cannot be reproduced faithfully on a
general-purpose kernel, and it is the reason the project targets QNX rather
than, say, a Linux daemon.

The measurement itself is taken by the control loop (`exec` covers the job's
work plus its two IPC round trips, i.e. what the loop actually spends per cycle) and independently by the supervisor (arrival times), so a task that
reports optimistic numbers still shows up in the supervisor's own slack figure
and in the sequence-gap counters.

## 3. Process structure

A resource manager is single-threaded for a reason: no internal locks means no
lock can be held by a low-priority thread while a high-priority one waits. The
supervisor follows that shape.

```
sentineld   one thread: dispatch_block() -> devctl / read / pulse handlers
            one extra thread: log writer at priority 5 (never above a task)

plant-sim   the supervised control loop (priority 20, 1 kHz)
loadgen     the rest of the system: navigation, vision, logging (priority 0..)
sentinel-ctl operator tool: open("/dev/sentinel"), devctl, read
```

Priorities (configuration, `etc/sentinel.conf`): supervisor 30, control loop 20,
everything else below. The invariant that makes the design work is
**supervisor priority > every supervised task priority**, which is the property
QNX guarantees and a GPOS does not.

## 4. Protocol

The client-facing API is a device, not a library:

| Operation | QNX | Host simulation |
|---|---|---|
| status snapshot | `read(fd)` → text, `devctl(SL_DCMD_STATUS)` → `sl_status_t` | same, over a datagram |
| register a task | `devctl(SL_DCMD_REGISTER, sl_policy_t)` | same |
| job start | `devctl(SL_DCMD_JOBSTART, sl_job_t)` → `sl_go_t` | same |
| job done | `devctl(SL_DCMD_JOBDONE, sl_job_t)` | same |
| operator reset / forced mode | `devctl(SL_DCMD_RESET / SETMODE)` | same |
| deadline expiry | pulse (`SL_PULSE_TIMER`, value = timer slot) | `timerfd` readable |

`SL_DCMD_JOBSTART` returns the current `sl_action_t` (RUN / THROTTLE / HOLD) and
mode. The control loop therefore learns about an escalation **in the same
message round trip as the job start**, with zero jobs of latency, instead of
polling a flag.

The message sizes are fixed and small (`sl_job_t` is 16 bytes), so the whole
data path is one `MsgSend`/`MsgReply` pair per job element — two round trips per
1 kHz cycle, about 2 000 messages per second, which is nothing for QNX.

## 5. Escalation ladder

```
          risk  >= 0.2 %          risk >= 2 %            critical miss
 NOMINAL ────────────────▶ WATCH ────────────────▶ DEGRADED ───────────▶ SAFE-STOP
    ▲                        │                          │                  │
    └──── 3 s clean ─────────┴──────────────────────────┘        latched: operator
          (no miss, risk clear)                                    reset required
```

* **WATCH** — the predictor says a miss is becoming likely. The control loop is
  throttled (the simulated loop drops its optional stage, using 35 % of the
  work) while nothing has actually been missed yet.
* **DEGRADED** — a critical task missed, or the predicted risk passed 2 %. Same
  throttling, plus the reason is recorded and reported.
* **SAFE-STOP** — `stop_after_misses` critical misses inside `miss_window` (3 in
  5 s by default). The control loop is told `HOLD`, freezes the actuator at its
  last safe setpoint, and stays there until an operator resets. This is
  deliberately not automatic: a safety state that clears itself is not a safety
  state.
* **Recovery** — from WATCH/DEGRADED back to NOMINAL needs the risk to be clear
  *and* no miss for `recover_after` (3 s), which prevents ladder chatter.

Every transition carries a reason code (`SL_RS_*`), so the log explains itself.

## 6. The predictor

Input: the ring of the last 1 024 execution times of a task and its budget.
Output: `risk_ppm`, the estimated probability that the next job exceeds the
budget, in parts per million.

Two windows are evaluated and the larger estimate wins:

* the whole ring (long-run behaviour),
* the most recent 128 samples (a regime change is visible long before 1 024
  samples have turned over).

Inside a window, with samples sorted:

* **budget at or below the 90th percentile** — the empirical exceedance rate is
  used directly. There is enough data in the tail, no extrapolation needed.
* **budget above the 90th percentile** — a peaks-over-threshold exponential tail
  is fitted above p90:

  ```
  P(X > b) ≈ (n_excess / n) · exp( -(b - p90) / scale )
  scale     = max( mean excess over p90 , 0.5 · (p99 - p50) , 1 µs )
  ```

  For a distribution that can be modelled as a bounded body plus rare stalls,
  the exponential tail is the right shape: it is exactly the distribution of
  "time beyond the noise band". The `0.5·(p99-p50)` floor matters: on a good RTOS
  the mean excess over p90 can be a few hundred nanoseconds, and a pure
  maximum-likelihood fit would then declare any budget above it to be
  *impossible* — which makes the predictor blind to the difference between
  "plenty of headroom" and "some headroom". The floor keeps the estimate finite
  and monotone in the headroom.

The estimate is integer-only (no `double` in the hot path except a single
exponential in an integer-planned expression) and allocation-free, and the same
code produces the numbers in the host report, so what is shown in a report can
be reproduced from the log.

## 7. The log

Requirements: it must never delay the real-time path, and it must survive a
power cut.

* Fixed-size records: `sl_rec_t` header (magic, type, length, sequence,
  monotonic timestamp) + payload + CRC-32 over both.
* Producer (supervisor thread) writes into a single-producer/single-consumer
  ring with `__atomic_store_n(..., __ATOMIC_RELEASE)`; it never touches a file
  descriptor, never allocates, and counts the records it had to drop if the disk
  falls behind (`sl_log_drops`).
* Consumer: one writer thread at priority 5 — below every task — which batches
  writes and flushes every 2 ms.
* Job records are decimated (`log_decimation`, one in four by default) because a
  1 kHz loop would otherwise write 32 kB/s; **misses, mode transitions and
  operator actions are never decimated**.
* The reader tolerates a truncated tail (stops cleanly) and skips corrupt
  records, counting them, rather than abandoning the file.

`sentinel-report` reads this format on any host, recomputes percentiles from the
raw job records, and renders an HTML report with a mode timeline, per-task
histograms and a transition table.

## 8. Threat and fault model

| Fault | What Sentinel does | How it was tested |
|---|---|---|
| job overruns its budget | timer fires, task is declared dead (in_flight cleared), miss recorded, ladder escalates | `test_sup.c`, `scenario.sh` |
| job never completes | same timer path — no cooperation required | `test_sup.c: test_timer_expiry_counts_as_miss` |
| job completes but reports no completion | overlapping-start is detected and counted as a policy violation | `test_sup.c: test_duplicate_and_unknown` |
| events are lost between task and supervisor | sequence gap is added to `events_lost` and shown in the status | `test_sup.c: test_sequence_gap_counts_lost` |
| a task pretends to be another | task id must be registered; unknown ids are rejected, not auto-registered | `test_duplicate_and_unknown` |
| malformed devctl message | length is taken from the received message (`ctp->size`), never from the client's claim; oversized requests are refused | `plat_qnx.c: rm_devctl` |
| log corruption / power cut | per-record CRC, clean stop at a truncated tail | `test_log.c` |
| the disk stalls | ring drops are counted, the real-time path is unaffected | `sl_log.c` |

## 9. What this is not (yet)

* No permanent storage of statistics across reboots; the log is the record.
* One supervisor per process (a resource manager is single-threaded).
* The simulated plant is a stand-in for a real joint or pump; the supervisor
  itself is plant-agnostic — it only sees jobs, budgets and deadlines.
* Priorities are set by the configuration file. A production deployment should
  use QNX abilities (`PROCMGR_AID_PRIORITY`, `PROCMGR_AID_PATHSPACE`,
  `PROCMGR_AID_PUBLIC_CHANNEL`) so the daemon holds exactly the privileges it
  needs; see `docs/PRIOR-ART.md`.
