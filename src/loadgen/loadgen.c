/*
 * loadgen - competing CPU load, used to create jitter on purpose.
 *
 * On the target this plays the part of the other work in a real system:
 * navigation, vision, logging, a misbehaving driver. Each worker burns for
 * `work_us` and sleeps for `sleep_us`, so `--work 400 --sleep 100` is an
 * 80% duty cycle on one CPU.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_plat.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_stop = 0;
static uint64_t g_work_ns = 400000u;
static uint64_t g_sleep_ns = 100000u;

static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
}

static void burn(uint64_t ns)
{
    uint64_t start = sl_now_ns();
    volatile double x = 1.0;
    while (sl_now_ns() - start < ns) {
        for (int i = 0; i < 500; i++) {
            x = x * 1.000001 + 0.25;
        }
    }
    (void)x;
}

static void *worker(void *arg)
{
    (void)arg;
    while (!g_stop) {
        burn(g_work_ns);
        if (g_sleep_ns) {
            struct timespec ts;
            ts.tv_sec = (time_t)(g_sleep_ns / 1000000000ull);
            ts.tv_nsec = (long)(g_sleep_ns % 1000000000ull);
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int threads = 2, i, opt;
    double duration_s = 0.0, prio = 0.0;
    pthread_t tid[64];
    double start;

    while ((opt = getopt(argc, argv, "n:d:w:s:r:h")) != -1) {
        switch (opt) {
        case 'n': threads = atoi(optarg); break;
        case 'd': duration_s = atof(optarg); break;
        case 'w': g_work_ns = (uint64_t)(atof(optarg) * 1e3); break;
        case 's': g_sleep_ns = (uint64_t)(atof(optarg) * 1e3); break;
        case 'r': prio = atof(optarg); break;
        default:
            printf("usage: %s [-n threads] [-d seconds] [-w work_us] [-s sleep_us] [-r prio]\n",
                   argv[0]);
            return 0;
        }
    }
    if (threads > 64) {
        threads = 64;
    }
    if (prio > 0.0) {
        sl_raise_priority((int)prio);
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    start = (double)sl_now_ns() / 1e9;
    for (i = 0; i < threads; i++) {
        if (pthread_create(&tid[i], NULL, worker, NULL) != 0) {
            fprintf(stderr, "loadgen: thread create failed\n");
            break;
        }
    }
    printf("loadgen: %d workers, work %llu us, sleep %llu us\n", i,
           (unsigned long long)(g_work_ns / 1000u),
           (unsigned long long)(g_sleep_ns / 1000u));
    fflush(stdout);

    while (!g_stop) {
        usleep(50000);
        if (duration_s > 0.0 && (double)sl_now_ns() / 1e9 - start >= duration_s) {
            break;
        }
    }
    g_stop = 1;
    for (i = 0; i < threads; i++) {
        pthread_join(tid[i], NULL);
    }
    printf("loadgen: stopped\n");
    return 0;
}
