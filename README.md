# Sentinel — deadline supervision for QNX 8.0

A real-time supervisor that watches every job of a control loop, proves whether
its deadline was met, predicts the miss **before** it happens, and puts the
actuator into a safe state when the timing envelope finally breaks.

Built for the [QNX Global Hackathon](https://qnx.devpost.com/) — category
**Robotics & Medical** — and it runs on the free QNX SDP 8.0 inside QEMU, so it
needs no target hardware.

```
                 ┌──────────────────────────────────────────────┐
   control loop  │  sentineld            /dev/sentinel          │   operator
   (1 kHz)       │  ┌────────────────┐   ┌────────────────────┐  │   tools
  ┌─────────┐    │  │ deadline timers│   │ resource manager   │  │  ┌──────────────┐
  │plant-sim├────┼─▶│ (POSIX timers  │   │ read()  -> status  │◀─┼──┤sentinel-ctl  │
  │ job     │    │  │  + SIGEV_PULSE)│   │ devctl()-> register│  │  │ status/reset │
  │ start   │    │  └───────┬────────┘   │            job/ctl │  │  └──────────────┘
  │ work    │    │          │ pulse      └────────────────────┘  │
  │ done    │    │  ┌───────▼────────┐                          │
  └─────────┘    │  │ supervisor     │  NOMINAL→WATCH→DEGRADED  │
                 │  │ state machine  │  →SAFE-STOP (latched)    │
                 │  └───────┬────────┘                          │
                 │  ┌───────▼────────┐   ┌────────────────────┐  │
                 │  │ RCU-free log   │──▶│ .slg event log     │──┼─▶ sentinel-report
                 │  │ ring, prio 5   │   │ CRC per record     │  │   (host: HTML/JSON)
                 │  └────────────────┘   └────────────────────┘  │
                 └──────────────────────────────────────────────┘
```

## Why this, and why QNX

A deadline miss in a robot joint or an infusion pump is not a performance
problem, it is a safety event. Linux (even PREEMPT_RT) can only tell you that
you were late; QNX **guarantees** the priority relationship that keeps the
enforcement path running while everything else is starving. Sentinel is built to
use exactly that:

* the supervisor runs at a higher priority than any task it watches, and its
  deadline timers are kernel POSIX timers whose expiry arrives as a **pulse** —
  the supervisor is woken by the kernel, not by a polling thread;
* it is a **resource manager** (`/dev/sentinel`), so tasks and tools talk to it
  with ordinary `open`/`devctl`/`read` while the data path stays a **native
  message** (no socket stack, no copy through a userspace library);
* the enforcement path is allocation-free and lock-free: a fixed timer per task,
  a fixed-size event-log ring drained by a **low-priority writer thread**;
* the service is single-threaded by design (a resource manager has no internal
  locks to invert), and priorities are per-task so a task cannot inherit a
  higher one by accident.

The interesting part is the middle of the ladder: Sentinel estimates the
probability that the *next* job exceeds its budget from the execution-time
distribution it has observed so far, and sheds optional work when that
probability rises — so the deadline is protected before it is missed.

## What it does

| | |
|---|---|
| Deadline enforcement | one absolute one-shot timer per job, armed at job start; a job that never completes is caught without any cooperation from the task |
| Escalation | `NOMINAL → WATCH → DEGRADED → SAFE-STOP`; SAFE-STOP is latched until an operator reset |
| Prediction | peaks-over-threshold tail model over the execution-time ring, evaluated on the whole ring and on the most recent 128 samples (a regime change is visible before the ring turns over) |
| Control decision | `RUN` / `THROTTLE` / `HOLD` is returned with the job-start acknowledgement, so the control loop learns the new mode in the same message round trip (zero-jobs latency) |
| Evidence | every job (decimated), every miss, every mode transition and every operator action is written to a CRC-protected log; `sentinel-report` turns it into an HTML/JSON report **on the host, with no QNX installed** |
| Detectability | lost events are counted through sequence gaps; overlapping or duplicate completions and unknown task ids are counted as policy violations instead of being silently absorbed |

## Quick start — host simulation (no QNX needed)

The supervisor, the statistics, the log and every tool are portable C11. A
POSIX backend replaces the channel with a `SOCK_SEQPACKET` socket and the timer
pulses with `timerfd`, so the whole system can be built and exercised on Linux
or macOS:

```sh
./tools/build-host.sh            # builds into build/
./tools/test.sh                  # unit tests
./tools/test.sh all              # unit tests + end-to-end scenario
./tools/demo.sh                  # the demo story, artifacts land in demo/
```

The host build is a development aid, not a real-time claim: on a general-purpose
kernel with an unprivileged process the numbers are dominated by host noise.
Use it to change behaviour, use QEMU to measure it.

## Quick start — QNX 8.0 in QEMU

1. Get the free non-commercial license at
   <https://qnx.software/en/developers/get-started/qnx-everywhere>
   (a myQNX account, then the QNX Everywhere license form).
2. Install the SDP and load its environment:

   ```sh
   . ~/qnx800/qnxsdp-env.sh
   ```

3. Cross-build:

   ```sh
   ./qnx/build-qnx.sh                 # -> build-qnx/  (x86_64 by default)
   ```

4. Create and start a QEMU guest (part of the SDP):

   ```sh
   mkdir qnx-vm && cd qnx-vm
   mkqnximage --type=qemu --build
   mkqnximage --run                   # add =-h for headless
   mkqnximage --getip                 # prints the guest address
   ```

5. Copy the binaries in and run the demo **on the guest**:

   ```sh
   ./qnx/deploy-to-qemu.sh <vm-ip>
   ssh root@<vm-ip>
   cd /tmp && ./run-demo.sh
   ```

6. Bring the event log back and render it on the host:

   ```sh
   scp root@<vm-ip>:/tmp/sentinel.slg .
   ./build/sentinel-report sentinel.slg -t 0=control -j report.json -h report.html
   ```

## Build and run instructions (what the submission form asks for)

* **QNX version**: QNX SDP 8.0 (QNX Everywhere, non-commercial license)
* **Target**: QEMU x86_64 guest made by `mkqnximage --type=qemu`, or an
  AArch64 board (build with `QNX_VARIANT=gcc_ntoaarch64le`)
* **Toolchain**: `qcc -Vgcc_ntox86_64` from the SDP
* **Host tooling** (optional): any C11 compiler; the report tool has no QNX
  dependency

## Evidence of QNX use

| File | What it does with QNX |
|---|---|
| `src/plat/plat_qnx.c` | `resmgr_attach()` of `/dev/sentinel`, `dispatch_create_channel()`, `pulse_attach()`, `timer_create(CLOCK_MONOTONIC, SIGEV_PULSE)`, `devctl()` client, `ChannelCreate()`, `ConnectAttach()`, `pthread_setschedparam(SCHED_FIFO)` + `mlockall()` |
| `include/sentinel.h` | `__DIOF/__DIOT/__DIOTF` devctl command encoding, `_PULSE_CODE_MINAVAIL`-range pulse codes |
| `src/sentineld/main.c` | single-threaded supervisor/dispatch loop; `io_devctl` and `io_read` handlers live in the platform file so the state machine stays portable |
| `src/lib/sl_log.c` | low-priority writer thread (`sl_log_set_thread_priority`) so logging cannot delay the real-time path |
| `qnx/build-qnx.sh` | `qcc` cross build for `gcc_ntox86_64` / `gcc_ntoaarch64le` |
| `qnx/run-demo.sh` | runs on the target: supervisor at priority 30, control loop at 20, competing load |

## Repository layout

```
include/        protocol and data model shared by target and host tools
src/lib/        supervisor state machine, statistics, log, config, clock
src/plat/       plat_qnx.c (resource manager + pulses) / plat_posix.c (host sim)
src/sentineld/  the supervisor daemon
src/plant/      1 kHz simulated control loop with scriptable faults
src/loadgen/    competing CPU load
src/ctl/        operator CLI (status / watch / reset / mode)
src/report/     host-side log analyser: console summary, JSON, HTML
tests/          unit tests (fake clock, fake timers) + end-to-end scenario
etc/            configuration
qnx/            cross build, deploy and on-target demo scripts
tools/          host build, test and demo scripts
docs/           design, codelab, demo script, submission text
```

## Tests

`tests/test_sup.c` drives the whole escalation ladder with a fake clock and
fake timers: no sleeps, no threads, no flakiness. `tests/test_log.c` proves the
log survives a corrupted record and a truncated tail (a power cut in the middle
of a write). `tests/test_stats.c` pins the percentile and risk behaviour,
including the regime-change case. `tools/scenario.sh` runs the real binaries
over the real IPC path and asserts that an injected fault produced an
escalation, a safe stop and a `HOLD` command.

## Licence

MIT. See [LICENSE](LICENSE).
