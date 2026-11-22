# Codelab: build a deadline checker on QNX 8.0 (no hardware, ~30 minutes)

You will build, in two small programs, the core of every hard-real-time
guarantee: a job starts, a kernel timer is armed for its deadline, and if the
job is not done when the timer fires, the system *knows* — without the job's
cooperation and without polling.

Everything here runs on a free QNX 8.0 virtual machine. No target hardware, no
commercial licence.

---

## 0. What you need

* A myQNX account and the free **QNX Everywhere** license:
  <https://qnx.software/en/developers/get-started/qnx-everywhere>
* The QNX SDP 8.0 installed (Linux host assumed here)
* QEMU, which the SDP drives for you with `mkqnximage`

Load the toolchain in every shell you build from:

```sh
. ~/qnx800/qnxsdp-env.sh
qcc -V
```

## 1. Start a QNX 8.0 VM

```sh
mkdir ~/qnx-lab && cd ~/qnx-lab
mkqnximage --type=qemu --build         # builds the image (a few minutes)
mkqnximage --run                       # or --run=-h for headless
mkqnximage --getip                     # prints the guest's address
```

You now have a real QNX 8.0 system. Everything else is cross-compiled on the
host and copied in with `scp`; `mkqnximage` sets up passwordless root SSH for
the current user.

```sh
scp pulse-demo root@<ip>:/tmp/
ssh root@<ip> /tmp/pulse-demo
```

## 2. A timer that wakes you with a pulse

On a GPOS you would poll, or block in a driver. On QNX a timer expiry can be
delivered **as a message** — a *pulse* — into a channel you own. That is a
kernel-level notification: no thread of yours has to be running, and the wake-up
happens at your process's priority.

```c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/neutrino.h>

#define PULSE_CODE _PULSE_CODE_MINAVAIL
#define PERIOD_NS  10000000L /* 10 ms */

int main(void)
{
    int            chid;
    int            coid;
    struct sigevent ev;
    timer_t        tid;
    struct itimerspec its;
    union { struct _pulse pulse; } msg;
    struct timespec last, now;
    unsigned long   ticks = 0;

    chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }

    /* A connection to our own channel: this is what the timer delivers to. */
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        return 1;
    }

    /* The event: "send pulse PULSE_CODE to coid". Nothing else needed. */
    SIGEV_PULSE_INT_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &tid) == -1) {
        perror("timer_create");
        return 1;
    }
    its.it_value.tv_sec     = 0;
    its.it_value.tv_nsec    = PERIOD_NS;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = PERIOD_NS;
    if (timer_settime(tid, 0, &its, NULL) == -1) {
        perror("timer_settime");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        rcvid_t rcvid = MsgReceive(chid, &msg, sizeof(msg), NULL);

        if (rcvid == -1) {
            perror("MsgReceive");
            break;
        }
        if (rcvid != 0) {
            continue;             /* a real message, not a pulse */
        }
        if (msg.pulse.code != PULSE_CODE) {
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        {
            long long delta = (long long)(now.tv_sec - last.tv_sec) * 1000000000LL +
                              (now.tv_nsec - last.tv_nsec);
            last = now;
            printf("tick %lu  interval %lld ns  jitter %lld ns\n",
                   ++ticks, delta, delta - PERIOD_NS);
        }
        if (ticks >= 20) {
            break;
        }
    }

    timer_delete(tid);
    ConnectDetach(coid);
    ChannelDestroy(chid);
    return 0;
}
```

Three things are worth noticing, because they are what a GPOS does not give you:

1. **`rcvid == 0` means "pulse".** A pulse is not a message with a sender; it is
   a notification. The kernel writes it into your channel and `MsgReceive`
   returns 0.
2. **`_PULSE_CODE_MINAVAIL` onwards is yours.** Codes below that belong to the
   system; using them breaks other services.
3. **The interval you print is a measurement, not a promise.** Run it on a busy
   machine and watch the jitter: that number is the reason real-time systems
   care who is allowed to preempt whom.

Build and run it:

```sh
qcc -o pulse-demo pulse-demo.c
scp pulse-demo root@<ip>:/tmp/ && ssh root@<ip> /tmp/pulse-demo
```

You should see twenty lines, intervals close to 10 000 000 ns.

> Make the timer periodic versus one-shot: set `it_interval` to zero and the
> timer fires once. That single change is the heart of deadline enforcement,
> which is what part 3 does.

## 3. A deadline checker as a resource manager

A checker that the control loop must call *into* needs an interface. On QNX the
natural one is a **resource manager**: a path in the namespace, `/dev/deadline`,
that behaves like a device. Any process can `open()` and `devctl()` it, no
library and no ABI of ours to link against.

Two commands are enough:

* `DCMD_JOB_START` (input: budget in nanoseconds) — arm a one-shot timer for
  `now + budget`;
* `DCMD_JOB_END` — disarm it.

If the timer fires before `DCMD_JOB_END`, the job missed its deadline.

```c
#include <devctl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <time.h>

#define SL_PULSE_TIMER  _PULSE_CODE_MINAVAIL
#define DCMD_JOB_START  __DIOT(_DCMD_MISC, 1, uint32_t)
#define DCMD_JOB_END    __DION(_DCMD_MISC, 2)
#define DCMD_JOB_STATS  __DIOF(_DCMD_MISC, 3, stats_t)

typedef struct {
    unsigned long long jobs;
    unsigned long long misses;
    unsigned long long worst_ns;
} stats_t;

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          attr;
static timer_t                job_timer;
static unsigned long long     deadline_ns;
static stats_t                stats;

static int on_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    struct _pulse *pulse = (struct _pulse *)ctp->msg;

    (void)flags; (void)handle;
    if (code == SL_PULSE_TIMER && pulse->value.sival_int == 1) {
        stats.misses++;
        printf("MISS: job did not finish inside %llu ns\n", deadline_ns);
        fflush(stdout);
    }
    return 0;
}

static int on_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    struct _io_devctl_reply *reply = (struct _io_devctl_reply *)ctp->msg;
    struct itimerspec its;
    int sts;

    if ((sts = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT) {
        return sts;
    }

    switch (msg->i.dcmd) {
    case DCMD_JOB_START: {
        uint64_t budget = *(uint32_t *)_IO_INPUT_PAYLOAD(msg);
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec  = (time_t)(budget / 1000000000ull);
        its.it_value.tv_nsec = (long)(budget % 1000000000ull);
        timer_settime(job_timer, 0, &its, NULL);
        deadline_ns = budget;
        stats.jobs++;
        break;
    }
    case DCMD_JOB_END:
        memset(&its, 0, sizeof(its));
        timer_settime(job_timer, 0, &its, NULL);   /* disarm */
        break;
    case DCMD_JOB_STATS: {
        stats_t *out = (stats_t *)_IO_OUTPUT_PAYLOAD(*reply);
        *out = stats;
        memset(reply, 0, sizeof(*reply));
        SETIOV(ctp->iov, reply, sizeof(*reply) + sizeof(stats));
        return _RESMGR_NPARTS(1);
    }
    default:
        return ENOSYS;
    }

    memset(reply, 0, sizeof(*reply));
    SETIOV(ctp->iov, reply, sizeof(*reply));
    return _RESMGR_NPARTS(1);
}

int main(void)
{
    dispatch_t         *dpp;
    dispatch_context_t *ctp;
    resmgr_attr_t       rattr;
    struct sigevent     ev;
    int                 chid, coid;

    chid = ChannelCreate(0);
    dpp  = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = on_devctl;
    iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    if (resmgr_attach(dpp, &rattr, "/dev/deadline", _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach");
        return 1;
    }

    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    SIGEV_PULSE_INT_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, SL_PULSE_TIMER, 1);
    timer_create(CLOCK_MONOTONIC, &ev, &job_timer);
    pulse_attach(dpp, 0, SL_PULSE_TIMER, on_pulse, NULL);

    ctp = dispatch_context_alloc(dpp);
    printf("deadline-rm: /dev/deadline ready\n");

    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            return 1;
        }
        dispatch_handler(ctp);
    }
}
```

Two details that trip everyone up the first time:

* `dispatch_create_channel(-1, ...)` would create a *private* channel that other
  processes cannot connect to. Create the channel yourself with
  `ChannelCreate(0)` and pass its id in, so `/dev/deadline` is reachable.
* The devctl data sits **after** the `io_devctl_t` header in the received
  message; `_IO_INPUT_PAYLOAD()` and `_IO_OUTPUT_PAYLOAD()` are the supported way
  to reach it, and the reply goes into the *same* buffer (`ctp->msg`), which is
  why the reply structure is written over the request.

## 4. Drive it from a client

```c
/* client.c */
#include <devctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define DCMD_JOB_START __DIOT(_DCMD_MISC, 1, uint32_t)
#define DCMD_JOB_END   __DION(_DCMD_MISC, 2)

int main(void)
{
    int fd = open("/dev/deadline", O_RDWR);
    uint32_t budget_ns = 900000;   /* 0.9 ms */
    int i;

    if (fd == -1) { perror("open"); return 1; }

    for (i = 0; i < 1000; i++) {
        devctl(fd, DCMD_JOB_START, &budget_ns, sizeof(budget_ns), NULL);
        /* ... the job's work goes here ... */
        devctl(fd, DCMD_JOB_END, NULL, 0, NULL);
        usleep(1000);
    }
    close(fd);
    return 0;
}
```

Run `deadline-rm` in one terminal and the client in another. To see a miss,
replace the work with `usleep(1500)` (1.5 ms) for a few iterations: the checker
prints `MISS` **while the client is still inside its job** — it does not wait for
the job to finish to know that it is late.

## 5. Where to go next

This codelab stops at detection. The full project, **Sentinel**, adds the parts
that make it usable on a real robot or medical device:

* a *ladder* of modes (WATCH → DEGRADED → SAFE-STOP) and an operator reset;
* **prediction** of the next overrun from the execution-time distribution, so
  the system degrades *before* the first miss;
* a CRC-protected event log written by a low-priority thread, so the real-time
  path never blocks on a disk, and a host-side report tool;
* a simulated 1 kHz control loop, a load generator, and scripted faults, so the
  whole story is repeatable.

<https://github.com/> (Sentinel repository link goes here)

Exercises, roughly in order of difficulty:

1. Make the checker track several tasks (one timer per task slot).
2. Add `read()` to the resource manager so `cat /dev/deadline` prints the
   counters.
3. Record `exec` time in the client and have the checker compute the worst slack.
4. Set the checker's priority with `pthread_setschedparam(SCHED_FIFO)` and add a
   CPU hog at a lower priority: the misses should disappear even though the CPU
   is busy — that is the whole point of an RTOS.
5. Run `pidin` and `hogs` in the guest and correlate what you see with the
   checker's counters.
