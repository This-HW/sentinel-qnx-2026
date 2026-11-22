/*
 * sl_clock.c - the monotonic clock, identical on QNX 8.0 and on POSIX hosts.
 *
 * CLOCK_MONOTONIC on QNX is backed by the microkernel's high-resolution
 * timer and is never adjusted by NTP, which is what a deadline supervisor
 * must measure against. On Linux it is clock_gettime(CLOCK_MONOTONIC).
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_plat.h"

#include <time.h>

uint64_t sl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sl_abs_ns_to_timespec(uint64_t abs_ns, void *ts_out)
{
    struct timespec *ts = (struct timespec *)ts_out;
    ts->tv_sec  = (time_t)(abs_ns / 1000000000ull);
    ts->tv_nsec = (long)(abs_ns % 1000000000ull);
}
