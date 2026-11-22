/*
 * test_stats.c - execution-time statistics and risk model.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_stats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static uint64_t scratch[SL_STATS_RING];

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void test_empty(void)
{
    sl_stats_t s;
    sl_stats_reset(&s);
    CHECK(sl_stats_count(&s) == 0, "empty count");
    CHECK(sl_stats_percentile(&s, 500000, scratch) == 0, "empty p50");
    CHECK(sl_stats_risk_ppm(&s, 1000, scratch) == 0, "empty risk");
    CHECK(sl_stats_mean(&s) == 0, "empty mean");
}

static void test_percentiles(void)
{
    sl_stats_t s;
    int i;

    sl_stats_reset(&s);
    for (i = 1; i <= 1000; i++) {
        sl_stats_push(&s, (uint64_t)i * 1000u);
    }
    CHECK(sl_stats_count(&s) == 1000, "count %zu", sl_stats_count(&s));
    CHECK(sl_stats_mean(&s) == 500500u, "mean %llu",
          (unsigned long long)sl_stats_mean(&s));
    {
        uint64_t p50 = sl_stats_percentile(&s, 500000, scratch);
        uint64_t p99 = sl_stats_percentile(&s, 990000, scratch);
        uint64_t p999 = sl_stats_percentile(&s, 999000, scratch);
        CHECK(p50 > 495000u && p50 < 506000u, "p50 %llu", (unsigned long long)p50);
        CHECK(p99 > 985000u && p99 < 996000u, "p99 %llu", (unsigned long long)p99);
        CHECK(p999 > 993000u && p999 <= 1000000u, "p999 %llu",
              (unsigned long long)p999);
    }
    CHECK(s.wcet == 1000000u, "wcet %llu", (unsigned long long)s.wcet);
}

static void test_ring_wraps(void)
{
    sl_stats_t s;
    int i;

    sl_stats_reset(&s);
    /* Fill well past the ring: the newest samples must be what remains. */
    for (i = 0; i < 5000; i++) {
        sl_stats_push(&s, (uint64_t)(i % 100) * 1000u);
    }
    CHECK(sl_stats_count(&s) == SL_STATS_RING, "ring count %zu", sl_stats_count(&s));
    CHECK(s.total == 5000, "total %llu", (unsigned long long)s.total);
    {
        uint64_t p50 = sl_stats_percentile(&s, 500000, scratch);
        CHECK(p50 > 45000u && p50 < 55000u, "ring p50 %llu",
              (unsigned long long)p50);
    }
}

static void test_stddev(void)
{
    sl_stats_t s;
    int i;

    sl_stats_reset(&s);
    for (i = 0; i < 100; i++) {
        sl_stats_push(&s, 420000u);
    }
    CHECK(sl_stats_stddev(&s) == 0, "flat stddev %llu",
          (unsigned long long)sl_stats_stddev(&s));

    sl_stats_reset(&s);
    for (i = 0; i < 50; i++) {
        sl_stats_push(&s, 400000u);
        sl_stats_push(&s, 600000u);
    }
    {
        uint64_t sd = sl_stats_stddev(&s);
        CHECK(sd > 95000u && sd < 105000u, "bimodal stddev %llu",
              (unsigned long long)sd);
    }
}

/* Risk must fall monotonically as budget grows, and must be negligible
 * once the budget is far outside anything the task has ever produced. */
static void test_risk_monotonic(void)
{
    sl_stats_t s;
    uint32_t seed = 12345;
    uint64_t r_tight, r_body, r_tail, r_far;
    int i;

    sl_stats_reset(&s);
    for (i = 0; i < 800; i++) {
        /* 300..500 us body with a 0.4% spike at 700 us */
        seed = seed * 1103515245u + 12345u;
        uint64_t v = 300000u + (seed >> 16) % 200000u;
        if ((seed & 0xFF) == 0) {
            v = 700000u;
        }
        sl_stats_push(&s, v);
    }
    r_tight = sl_stats_risk_ppm(&s, 350000u, scratch); /* inside the body */
    r_body  = sl_stats_risk_ppm(&s, 380000u, scratch); /* just past p90  */
    r_tail  = sl_stats_risk_ppm(&s, 420000u, scratch); /* into the tail  */
    r_far   = sl_stats_risk_ppm(&s, 900000u, scratch); /* far outside    */

    CHECK(r_tight > r_body, "risk must fall as budget grows: %llu -> %llu",
          (unsigned long long)r_tight, (unsigned long long)r_body);
    CHECK(r_body > r_tail, "risk must fall as budget grows: %llu -> %llu",
          (unsigned long long)r_body, (unsigned long long)r_tail);
    CHECK(r_tail > r_far, "risk must fall as budget grows: %llu -> %llu",
          (unsigned long long)r_tail, (unsigned long long)r_far);
    CHECK(r_far < 1000u, "risk at 900us should be negligible: %llu",
          (unsigned long long)r_far);
    CHECK(r_tail < 1000000u, "risk must be capped at 1e6");
}

/* A distribution that is entirely above the budget must predict ~certainty. */
static void test_risk_certain(void)
{
    sl_stats_t s;
    int i;

    sl_stats_reset(&s);
    for (i = 0; i < 200; i++) {
        sl_stats_push(&s, 1500000u + (uint64_t)i * 1000u);
    }
    CHECK(sl_stats_risk_ppm(&s, 1000000u, scratch) >= 990000u,
          "certain miss not detected");
}

/* The recent window must dominate: after a regime change the estimate
 * moves before the whole ring is repopulated. */
static void test_risk_regime_change(void)
{
    sl_stats_t s;
    uint64_t before, after;
    int i;

    sl_stats_reset(&s);
    for (i = 0; i < 1000; i++) {
        sl_stats_push(&s, 400000u + (uint64_t)(i % 20) * 1000u);
    }
    /* Budget at 600 us: no sample reaches it in the fast regime. */
    before = sl_stats_risk_ppm(&s, 600000u, scratch);

    /* 130 samples of a much slower regime (ring still mostly fast). */
    for (i = 0; i < 130; i++) {
        sl_stats_push(&s, 650000u + (uint64_t)(i % 20) * 1000u);
    }
    after = sl_stats_risk_ppm(&s, 600000u, scratch);
    CHECK(after > before, "regime change not reflected: %llu -> %llu",
          (unsigned long long)before, (unsigned long long)after);
}

int main(void)
{
    test_empty();
    test_percentiles();
    test_ring_wraps();
    test_stddev();
    test_risk_monotonic();
    test_risk_certain();
    test_risk_regime_change();

    printf("test_stats: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
