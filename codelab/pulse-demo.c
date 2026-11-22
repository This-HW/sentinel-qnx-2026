/*
 * pulse-demo.c - the smallest useful QNX timer: a periodic POSIX timer whose
 * expiry is delivered as a pulse into your own channel.
 *
 * Part of the Sentinel codelab (docs/CODELAB.md).
 *
 *   qcc -o pulse-demo pulse-demo.c
 *   ./pulse-demo
 *
 * SPDX-License-Identifier: MIT
 */
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
    union {
        struct _pulse pulse;
        /* other message types you may receive go here */
    } msg;
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
            continue; /* not a pulse: a real message would be handled here */
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
