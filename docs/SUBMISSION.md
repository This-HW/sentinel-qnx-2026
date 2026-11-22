# Devpost submission — text and checklist

Hackathon: <https://qnx.devpost.com/> · Rules: <https://qnx.devpost.com/rules>
Deadline: **2026-12-15 14:00 EST = 2026-12-16 04:00 KST** · All material in English.
Category: **Robotics & Medical**.
Bonus entries: **Best Codelab / Tutorial**, **Best use of QNX-specific Technologies**.

---

## Title

**Sentinel — deadline supervision for QNX 8.0**

## Elevator pitch (the one-liner field)

A real-time supervisor that enforces every control-loop job's deadline with a
kernel timer, predicts a miss before it happens from the observed execution-time
distribution, and latches the actuator into a safe state when the timing envelope
breaks — built on QNX 8.0 with native message passing, resource managers and
pulses, running in QEMU.

## Description (paste into "About the project")

### What it does

A robot joint controller, a motor drive, an infusion pump: all of them are only
correct if each control job finishes inside its period. Sentinel makes that
property observable and enforceable.

* **A job starts** → the supervisor arms a one-shot kernel timer for
  `start + budget`.
* **The job ends inside budget** → the timer is disarmed, execution time and
  slack are recorded, and the sample feeds a statistical model of that task.
* **The timer fires first** → the job has missed its deadline. The supervisor
  knows this without the task's cooperation: a task stuck in a driver, spinning,
  or preempted cannot report its own overrun, so it is not asked to.
* **The predicted risk of the next overrun rises** → the system moves to WATCH
  and then DEGRADED, and the control loop is told to shed its optional work
  *before* the first miss.
* **Critical misses keep coming** → SAFE-STOP. The control loop is told `HOLD`,
  the actuator freezes at its last safe setpoint, and the state latches until an
  operator resets it.

### Why QNX, specifically

The enforcement path has to run while everything else is starving, and it has to
be woken by the kernel.

* **Resource manager**: Sentinel publishes `/dev/sentinel`. Supervised tasks and
  operator tools use ordinary `open()` / `devctl()` / `read()`; the data path is
  one native message per job element. No socket stack, no shim library, no ABI
  of ours to link against.
* **Pulses**: each deadline timer is a POSIX timer created with
  `SIGEV_PULSE_INT_INIT` whose expiry is delivered as a pulse into the resource
  manager's channel. The supervisor is woken at its own priority at the moment of
  the deadline — no polling thread, no sleep, no missed window between checks.
* **Priorities**: the supervisor runs at 30, the control loop at 20, the log
  writer at 5. The ladder only works because QNX guarantees that relationship;
  on a general-purpose kernel the same code measures the noise of the machine
  instead of the deadline of the task.
* **Single-threaded service by design**: no internal locks, so no lock can be
  held by a low-priority thread while a high-priority one waits. All state lives
  in one dispatch loop; the only other thread is the log writer, deliberately
  below every task.
* **Allocation-free real-time path**: fixed timer per task slot, fixed-size log
  ring with release/acquire ordering, no `malloc` after startup.

### The prediction

The supervisor keeps a 1 024-sample ring of execution times per task, and
estimates the probability that the next job exceeds its budget, using the whole
ring and the most recent 128 samples (so a regime change is visible before the
ring turns over). Below the 90th percentile it uses the empirical exceedance
rate; above it, a peaks-over-threshold exponential tail with a jitter-based scale
floor, which keeps the estimate finite and monotone on a platform where the mean
excess over p90 can be a few hundred nanoseconds. The number is reported in parts
per million and drives the WATCH/DEGRADED thresholds.

This is the part that turns a watchdog into a control: Sentinel reports a miss,
but its job is to make the miss not happen.

### Evidence, not claims

* Every job (decimated 1:4), every miss, every mode transition and every
  operator action is written to a CRC-protected log. The writer runs at priority
  5 through a single-producer ring, so the real-time path can only ever *drop*
  records (and count the drops), never block.
* `sentinel-report` is a **host tool** (Windows/macOS/Linux, no QNX needed). It
  reads the log, recomputes percentiles from raw records, and renders an HTML
  report: mode timeline, per-task histograms with the budget line, transition
  table with reasons.
* Faults are scriptable, so the demo is reproducible: a 3 ms compute overrun, a
  40 ms blocking stall, a 4 s burst of random overruns.
* Sequence gaps, duplicate completions, overlapping jobs and unknown task ids
  are counted as `events_lost` / `policy_violations` — the system reports its own
  blind spots instead of hiding them.

### How it is built

* `sentineld` — supervisor daemon: resource manager, deadline timers, state
  machine, event log.
* `plant-sim` — a simulated 1 kHz control loop with a 0.9 ms budget and a
  scriptable fault timeline; it is the "robot" in the demo.
* `loadgen` — competing CPU load, the rest of the system.
* `sentinel-ctl` — operator tool: `status`, `watch`, `reset`, `mode`.
* `sentinel-report` — host-side log analysis (HTML + JSON).

The supervisor core, the statistics, the log format and every tool are portable
C11 with no dependency outside libc/libm; a POSIX backend (AF_UNIX + timerfd)
lets the whole system build and be tested on a laptop, and the QNX backend is the
primary target. Unit tests drive the escalation ladder with a fake clock and fake
timers (no sleeps, no flakiness); an end-to-end scenario runs the real binaries
over the real IPC path and asserts that an injected fault produced an escalation,
a safe stop and a `HOLD` command.

### Build and run

* QNX version: **QNX SDP 8.0** (QNX Everywhere, free non-commercial)
* Target: **QEMU x86_64** (`mkqnximage --type=qemu`), or AArch64 with
  `QNX_VARIANT=gcc_ntoaarch64le`
* Toolchain: `qcc -Vgcc_ntox86_64`
* `./qnx/build-qnx.sh`, `./qnx/deploy-to-qemu.sh <vm-ip>`, then `./run-demo.sh`
  on the guest. Host: `./tools/build-host.sh`, `./tools/test.sh all`.

### What is next

Adaptive partitioning integration (let the supervisor ask the system for a
larger budget when a task's measured distribution justifies it), plus a
`PROCMGR_AID_*` ability profile so the daemon ships with the least privilege it
can run on, and log replay into the same report tool for post-incident analysis.

## Fields on the submission form

| Field | What to put |
|---|---|
| Project name | Sentinel — deadline supervision for QNX 8.0 |
| Tagline | Deadline enforcement and pre-miss prediction for real-time control loops on QNX 8.0 |
| Category | **Robotics & Medical** |
| Repository URL | public GitHub repo, MIT licence visible in the About section |
| Video URL | the 3-minute YouTube link (`docs/DEMO.md`) |
| "Try it out" link | repository README (build & run section) |
| Built with | QNX 8.0, QNX SDP, C, QEMU, POSIX |
| Best Codelab/Tutorial URL | published codelab URL (`docs/CODELAB.md`) |
| Upstream project URL (ports only) | — (not applicable) |

## Checklist before pressing submit

- [ ] Repository is public, `LICENSE` at the root, licence visible in the About
      box on the repository page.
- [ ] `README.md` states the QNX version, the target (QEMU) and the toolchain.
- [ ] The repository **genuinely builds for QNX**: the demo video shows the
      binaries running inside the QNX guest, and `src/plat/plat_qnx.c` is the
      QNX integration.
- [ ] Video is public on YouTube, under 3:00, no third-party trademarks or music.
- [ ] Text description is in English (everything in the repo already is).
- [ ] Codelab/tutorial URL is public and was written during the submission
      period.
- [ ] A repository link is not a fork of an upstream project (not applicable —
      everything here is new).
- [ ] Optional: a `docs/screenshots/` folder with 2–3 images from the report and
      the terminal, for the submission gallery.

## Notes that protect the entry

* Do **not** claim hard numbers from the host simulation as QNX numbers. The
  host build is documented as a development aid; measurements in the video and
  text must come from the QEMU guest.
* The project must be newly created during the submission period
  (2026-11-02 – 2026-12-15). Keep the first commit inside that window.
* One submission per entry; multiple entries are allowed only if substantially
  different. This is a single submission.
