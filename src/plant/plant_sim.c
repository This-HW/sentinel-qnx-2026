/*
 * plant_sim - a simulated control loop that is supervised by sentineld.
 *
 * Every cycle is a real-time job: it asks the supervisor for permission
 * (JOBSTART), does a bounded amount of work, and reports the result
 * (JOBDONE). The loop holds itself to an absolute schedule so that drift
 * never accumulates; a late wake-up shows up as a short cycle, exactly as
 * it would on a real plant.
 *
 * Faults can be injected on a timeline so a demo is repeatable:
 *   -f at=4:overrun:3ms     extra compute pushes the job over its budget
 *   -f at=6:stall:40ms      a blocking call (driver/PCIe/peripheral)
 *   -f at=9:burst:2s:800us  randomly sized overruns for 2 s
 *
 * For an interactive demo: SIGUSR1 injects a 3 ms overrun, SIGUSR2 a 40 ms
 * blocking stall.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentinel.h"
#include "sl_plat.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    double   at_s;
    double   length_s;  /* burst only */
    double   size_ns;
    int      kind;      /* 0 overrun, 1 stall, 2 burst */
    int      used;      /* burst: is it currently active */
    double   end_s;
} fault_t;

enum { F_OVERRUN = 0, F_STALL = 1, F_BURST = 2 };

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_manual = 0;        /* SIGUSR1: one 3 ms overrun */
static volatile sig_atomic_t g_manual_stall = 0;  /* SIGUSR2: one 40 ms stall  */

static void on_sig(int s)
{
    if (s == SIGUSR1) {
        g_manual = 1;
    } else if (s == SIGUSR2) {
        g_manual_stall = 1;
    } else {
        g_stop = 1;
    }
}

/* Spin for `ns` nanoseconds of wall time: a stand-in for real compute
 * whose duration we control precisely. */
static void burn_ns(uint64_t ns)
{
    uint64_t start = sl_now_ns();
    volatile double x = 1.0;
    while (sl_now_ns() - start < ns) {
        for (int i = 0; i < 200; i++) {
            x = x * 1.000001 + 0.5;
        }
    }
    (void)x;
}

static void sleep_ns(uint64_t ns)
{
    struct timespec ts;
    sl_abs_ns_to_timespec(sl_now_ns() + ns, &ts);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

/* xorshift: deterministic variability, no libc rand() lock. */
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *name = "control";
    double period_ms = 1.0, budget_ms = 0.9, work_ms = 0.35, duration_s = 0.0;
    double prio = 20.0;
    fault_t faults[16];
    int nfaults = 0;
    sl_client_t *c = NULL;
    sl_policy_t policy;
    int task = -1;
    int opt;
    double start_s;
    uint32_t seq = 0;
    uint64_t last_report = 0;
    double prev_action = -1.0;

    while ((opt = getopt(argc, argv, "p:n:P:B:w:d:f:r:h")) != -1) {
        switch (opt) {
        case 'p': path = optarg; break;
        case 'n': name = optarg; break;
        case 'P': period_ms = atof(optarg); break;
        case 'B': budget_ms = atof(optarg); break;
        case 'w': work_ms = atof(optarg); break;
        case 'd': duration_s = atof(optarg); break;
        case 'r': prio = atof(optarg); break;
        case 'f': {
            fault_t f;
            char kind[32];
            double ms = 0.0, len = 0.0;
            memset(&f, 0, sizeof(f));
            if (sscanf(optarg, "at=%lf:%31[^:]:%lf:%lf", &f.at_s, kind, &ms, &len) >= 2) {
                f.kind = (strcmp(kind, "stall") == 0) ? F_STALL
                       : (strcmp(kind, "burst") == 0) ? F_BURST
                                                      : F_OVERRUN;
                f.size_ns = (strcmp(kind, "burst") == 0 ? ms * 1e3 : ms * 1e6);
                f.length_s = len;
                faults[nfaults++] = f;
            }
            break;
        }
        default:
            printf("usage: %s [-p path] [-n name] [-P period_ms] [-B budget_ms]\n"
                   "          [-w work_ms] [-d seconds] [-f at=SEC:kind:size[:len]]\n",
                   argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_sig);
    signal(SIGUSR2, on_sig);

    if (prio > 0.0 && sl_raise_priority((int)prio) < 0) {
        printf("plant: priority %d not granted, running as-is\n", (int)prio);
    }

    if (sl_client_open(&c, path) != 0) {
        fprintf(stderr, "plant: no supervisor at %s\n", sl_default_path());
        return 1;
    }

    memset(&policy, 0, sizeof(policy));
    snprintf(policy.name, sizeof(policy.name), "%s", name);
    policy.period_ns = (uint64_t)(period_ms * 1e6);
    policy.budget_ns = (uint64_t)(budget_ms * 1e6);
    policy.slack_ns  = (uint64_t)((budget_ms - work_ms) * 1e6);
    policy.priority  = (uint32_t)prio;
    policy.critical  = 1;

    if (sl_client_call(c, SL_DCMD_REGISTER, &policy, sizeof(policy),
                       &task, sizeof(task), NULL) != 0 || task < 0) {
        fprintf(stderr, "plant: registration refused\n");
        return 1;
    }
    printf("plant: registered '%s' as task %d (period %.3f ms, budget %.3f ms)\n",
           name, task, period_ms, budget_ms);
    fflush(stdout);

    start_s = (double)sl_now_ns() / 1e9;

    while (!g_stop) {
        sl_job_t job;
        sl_go_t  go;
        uint64_t t0, t1, exec;
        double now_s;
        uint64_t extra = 0, stall = 0;
        int i;

        /* Absolute schedule: cycle n starts at start + n*period. */
        {
            double target = start_s + (double)seq * (period_ms / 1000.0);
            struct timespec ts;
            uint64_t target_ns = (uint64_t)(target * 1e9);
            sl_abs_ns_to_timespec(target_ns, &ts);
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
            }
        }

        job.task = (uint32_t)task;
        job.seq = seq;
        job.exec_ns = 0;
        t0 = sl_now_ns();
        if (sl_client_call(c, SL_DCMD_JOBSTART, &job, sizeof(job),
                           &go, sizeof(go), NULL) != 0) {
            fprintf(stderr, "plant: supervisor refused job %u\n", seq);
            break;
        }

        now_s = (double)t0 / 1e9 - start_s;

        /* Fault timeline. */
        for (i = 0; i < nfaults; i++) {
            fault_t *f = &faults[i];
            if (f->kind == F_BURST) {
                if (!f->used && now_s >= f->at_s) {
                    f->used = 1;
                    f->end_s = now_s + f->length_s;
                }
                if (f->used && now_s < f->end_s) {
                    if ((rnd() % 100u) < 60u) {
                        extra += f->size_ns + (rnd() % 400u) * 1000u;
                    }
                }
            } else if (now_s >= f->at_s && !f->used) {
                f->used = 1;
                if (f->kind == F_STALL) {
                    stall += (uint64_t)f->size_ns;
                } else {
                    extra += (uint64_t)f->size_ns;
                }
            }
        }
        if (g_manual) {
            g_manual = 0;
            extra += 3000000ull; /* 3 ms one-shot overrun */
        }
        if (g_manual_stall) {
            g_manual_stall = 0;
            stall += 40000000ull; /* 40 ms blocking stall */
        }

        if (go.action == SL_ACTION_HOLD) {
            /* Safe state: actuator held, only the witness job runs. */
            burn_ns(20000u);
        } else if (go.action == SL_ACTION_THROTTLE) {
            burn_ns((uint64_t)(work_ms * 0.35 * 1e6) + extra);
        } else {
            burn_ns((uint64_t)(work_ms * 1e6) + (rnd() % 60u) * 1000u + extra);
        }
        if (stall) {
            sleep_ns(stall);
        }

        t1 = sl_now_ns();
        exec = t1 - t0;
        job.exec_ns = exec;
        (void)sl_client_call(c, SL_DCMD_JOBDONE, &job, sizeof(job), NULL, 0, NULL);

        if (prev_action != (double)go.action) {
            printf("plant: action -> %s (mode %s) at t=%.3fs\n",
                   go.action == SL_ACTION_RUN ? "RUN"
                   : go.action == SL_ACTION_THROTTLE ? "THROTTLE" : "HOLD",
                   sl_mode_name((int)go.mode), now_s);
            fflush(stdout);
            prev_action = (double)go.action;
        }

        seq++;
        if (seq % 1000u == 0u && sl_now_ns() - last_report > 900000000ull) {
            last_report = sl_now_ns();
        }
        if (duration_s > 0.0 && now_s >= duration_s) {
            break;
        }
    }

    printf("plant: stopped after %u jobs\n", seq);
    sl_client_close(c);
    return 0;
}
