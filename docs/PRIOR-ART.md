# Prior art, and what Sentinel adds

An honest review of what already exists, so the submission does not claim to
invent scheduling theory. Sentinel is not a scheduler and does not compete with
the QNX microkernel's scheduler: it is an *enforcement, prediction and evidence*
layer that sits on top of it.

## What already exists

**Watchdog timers and hardware watchdogs** (including QNX's own watchdog
services, `systemd`'s `WatchdogSec=`, Linux's `softlockup`/`hung_task`).
They answer "is this process alive". They do not answer "did this job finish
inside its 0.9 ms budget", and they act on the whole process, usually by
restarting it. A restarted robot joint is not in a safe state.

**Tracing and profiling** (QNX `tracelogger` / System Profiler, LTTng, perf).
Excellent for finding *why* a system was late, offline. They are not an
enforcement path: they produce data after the fact, and the instrumentation
itself is not something you want on the critical path of a 1 kHz loop.

**QNX adaptive partitioning / thread scheduler controls**. These are the
mechanisms that make a system schedulable in the first place. Sentinel assumes
them and reports on them; it does not replace them.

**PREEMPT_RT Linux, Xenomai, Zephyr, FreeRTOS**. Each has a way to run periodic
tasks with priorities. What they generally lack is a standard way to *observe*
per-job deadline compliance in production with evidence an auditor accepts, and
a safe-state protocol that the application gets told about. PREEMPT_RT also does
not give a guarantee for the enforcement path itself: the tool that measures can
be preempted by the thing it is measuring.

**Academic real-time monitoring**: deadline-miss detectors (e.g. the "RT-Monitor"
style work), and probabilistic WCET / execution-time analysis, are well
established. Sentinel uses the same statistical idea (peaks-over-threshold
tail estimation) but applies it online, in the supervisor, to decide a mode
change — which is an engineering choice, not a research contribution.

**ROS 2 real-time tooling** (`ros2_control`, real-time loops, `rclcpp` executors)
targets Linux with best-effort scheduling; on QNX the interesting work is
usually the integration layer, not deadline enforcement.

## What this project contributes

1. **Per-job enforcement that does not trust the task.** One kernel timer per
   running job, armed at start; expiry is a pulse into the supervisor's channel.
   A task that never returns is caught anyway, and one that lies about its own
   timing is caught by the supervisor's independent slack figure and sequence
   accounting.
2. **Pre-miss degradation instead of post-miss logging.** The mode ladder is
   driven by a predicted overrun probability computed from the task's own
   execution-time history, so the system sheds optional work while the deadline
   is still being met. The control loop learns the new mode in the same message
   round trip as its job start.
3. **A safety state that is a state.** SAFE-STOP latches and holds the actuator;
   recovery is an explicit, logged operator action. Automatic clearing is
   treated as a defect, not a feature.
4. **Evidence as a first-class output.** A CRC-protected, decimated, allocation-
   free event log plus a host-side report tool that needs no QNX installation:
   the artifact an auditor or a post-incident review asks for.
5. **The whole thing is testable without hardware.** A POSIX backend plus a
   scriptable plant and fault timeline make the safety path reproducible in CI;
   the QNX backend is a resource manager of about 400 lines on top of the same
   portable core.

## Deliberate non-goals

* It does not change scheduling policy or claim to make a non-schedulable system
  schedulable.
* It does not measure WCET statically; it measures what happened and extrapolates
  the tail, which is a different (and weaker) statement that is honest about
  itself: `risk_ppm` is an estimate, and the log keeps the samples it was based
  on.
* It is not a certification artifact. It is the kind of data a certification
  process would want, produced by a system small enough to reason about.
