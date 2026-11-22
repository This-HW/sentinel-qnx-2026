/*
 * deadline-rm.c - a 120-line deadline checker built as a QNX resource manager.
 *
 * Part of the Sentinel codelab (docs/CODELAB.md).
 *
 *   qcc -o deadline-rm deadline-rm.c
 *   ./deadline-rm &                 # publishes /dev/deadline
 *   Deadline-rm registers a device that any process can talk to with
 *   devctl(): start_job() arms a timer for that job, end_job() disarms it,
 *   and if the timer fires first the job is reported as a miss.
 *
 * SPDX-License-Identifier: MIT
 */
#include <devctl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <time.h>
#include <unistd.h>

#define SL_PULSE_TIMER  _PULSE_CODE_MINAVAIL
#define DCMD_JOB_START  __DIOT(_DCMD_MISC, 1, uint32_t) /* budget in ns  */
#define DCMD_JOB_END    __DION(_DCMD_MISC, 2)           /* no data       */
#define DCMD_JOB_STATS  __DIOF(_DCMD_MISC, 3, stats_t)  /* read counters */

typedef struct {
    unsigned long long jobs;
    unsigned long long misses;
    unsigned long long worst_ns;
} stats_t;

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          attr;

static timer_t        job_timer;
static int            job_running;
static unsigned long long deadline_ns;
static stats_t        stats;

static int on_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    struct _pulse *pulse = (struct _pulse *)ctp->msg;

    (void)flags;
    (void)handle;
    if (code == SL_PULSE_TIMER && pulse->value.sival_int == 1) {
        stats.misses++;
        job_running = 0;
        printf("MISS: job did not finish inside %llu ns\n",
               (unsigned long long)deadline_ns);
        fflush(stdout);
    }
    return 0;
}

static int on_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    struct _io_devctl_reply *reply = (struct _io_devctl_reply *)ctp->msg;
    int sts;

    if ((sts = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT) {
        return sts;
    }

    switch (msg->i.dcmd) {
    case DCMD_JOB_START: {
        struct itimerspec its;
        uint64_t budget = *(uint32_t *)_IO_INPUT_PAYLOAD(msg);

        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec  = (time_t)(budget / 1000000000ull);
        its.it_value.tv_nsec = (long)(budget % 1000000000ull);
        timer_settime(job_timer, 0, &its, NULL);
        deadline_ns = budget;
        job_running = 1;
        stats.jobs++;
        break;
    }
    case DCMD_JOB_END: {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        timer_settime(job_timer, 0, &its, NULL); /* disarm */
        job_running = 0;
        break;
    }
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
    int                 id;
    int                 chid;
    int                 coid;

    chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }
    dpp = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);
    if (dpp == NULL) {
        perror("dispatch_create_channel");
        return 1;
    }

    io_funcs.devctl = on_devctl;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.devctl = on_devctl;
    iofunc_attr_init(&attr, S_IFNAM | 0666, 0, 0);

    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    id = resmgr_attach(dpp, &rattr, "/dev/deadline", _FTYPE_ANY, 0,
                       &connect_funcs, &io_funcs, &attr);
    if (id == -1) {
        perror("resmgr_attach");
        return 1;
    }

    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    SIGEV_PULSE_INT_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, SL_PULSE_TIMER, 1);
    if (timer_create(CLOCK_MONOTONIC, &ev, &job_timer) == -1) {
        perror("timer_create");
        return 1;
    }
    pulse_attach(dpp, 0, SL_PULSE_TIMER, on_pulse, NULL);

    ctp = dispatch_context_alloc(dpp);
    printf("deadline-rm: /dev/deadline ready\n");
    fflush(stdout);

    for (;;) {
        if ((ctp = dispatch_block(ctp)) == NULL) {
            perror("dispatch_block");
            return 1;
        }
        dispatch_handler(ctp);
    }
}
