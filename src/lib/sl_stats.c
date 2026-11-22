/*
 * sl_stats.c - execution-time statistics and deadline-risk estimation.
 *
 * Integer-only, allocation-free: this runs inside a real-time process and
 * must never call malloc or take a lock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_stats.h"

#include <math.h>
#include <string.h>

void sl_stats_reset(sl_stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

void sl_stats_push(sl_stats_t *s, uint64_t sample_ns)
{
    s->ring[s->head] = sample_ns;
    s->head = (s->head + 1u) % SL_STATS_RING;
    if (s->count < SL_STATS_RING) {
        s->count++;
    }
    s->total++;
    s->sum += sample_ns;
    s->sumsq += sample_ns * sample_ns;
    if (sample_ns > s->wcet) {
        s->wcet = sample_ns;
    }
}

size_t sl_stats_count(const sl_stats_t *s)
{
    return (size_t)s->count;
}

uint64_t sl_stats_mean(const sl_stats_t *s)
{
    if (s->total == 0u) {
        return 0u;
    }
    return s->sum / s->total;
}

uint64_t sl_stats_stddev(const sl_stats_t *s)
{
    uint64_t n, mean, var;

    if (s->total < 2u) {
        return 0u;
    }
    n = s->total;
    mean = s->sum / n;
    /* E[X^2] - E[X]^2, clamped at zero for negative rounding. */
    var = (s->sumsq / n);
    if (var <= mean * mean) {
        return 0u;
    }
    var -= mean * mean;
    /* Integer square root, Newton's method. */
    {
        uint64_t x = var, y = (x + 1u) / 2u;
        while (y < x) {
            x = y;
            y = (x + var / x) / 2u;
        }
        return x;
    }
}

/* Insertion sort of a small window; n is at most SL_STATS_WINDOW. */
static void sort_u64(uint64_t *v, size_t n)
{
    size_t i, j;
    for (i = 1; i < n; i++) {
        uint64_t key = v[i];
        j = i;
        while (j > 0u && v[j - 1u] > key) {
            v[j] = v[j - 1u];
            j--;
        }
        v[j] = key;
    }
}

/* Copy the newest `want` samples into dst, newest-first order not required. */
static size_t snapshot(const sl_stats_t *s, uint64_t *dst, size_t want)
{
    size_t n = (size_t)s->count;
    size_t i;
    if (want > n) {
        want = n;
    }
    /* Ring index of the oldest of the newest `want` samples. */
    for (i = 0; i < want; i++) {
        size_t idx = (size_t)((s->head + SL_STATS_RING - want + i) % SL_STATS_RING);
        dst[i] = s->ring[idx];
    }
    return want;
}

static uint64_t percentile_sorted(const uint64_t *v, size_t n, uint32_t pct_ppm)
{
    uint64_t pos;
    size_t   lo, hi;
    uint64_t frac;

    if (n == 0u) {
        return 0u;
    }
    if (n == 1u) {
        return v[0];
    }
    /* pos = pct * (n-1) in fixed point with 1e6 scale */
    pos = (uint64_t)pct_ppm * (uint64_t)(n - 1u);
    lo = (size_t)(pos / 1000000u);
    hi = (lo + 1u < n) ? lo + 1u : lo;
    frac = pos % 1000000u;
    return v[lo] + (uint64_t)(((double)(v[hi] - v[lo]) * (double)frac) / 1000000.0);
}

uint64_t sl_stats_percentile(sl_stats_t *s, uint32_t pct_ppm, uint64_t *scratch)
{
    size_t n = snapshot(s, scratch, SL_STATS_RING);
    if (n == 0u) {
        return 0u;
    }
    sort_u64(scratch, n);
    return percentile_sorted(scratch, n, pct_ppm);
}

#define SL_STATS_WINDOW 128u

static uint64_t risk_window(uint64_t *sorted, size_t n, uint64_t budget_ns)
{
    uint64_t p50, p90, p99, excess_sum = 0u, excess_n = 0u, base_n, est, emp;
    size_t   i, first_excess = n;

    if (n < 8u) {
        /* Too few samples to say anything; report the empirical rate. */
        uint64_t over = 0u;
        for (i = 0; i < n; i++) {
            if (sorted[i] > budget_ns) {
                over++;
            }
        }
        return n ? (over * 1000000u) / n : 0u;
    }

    p50 = percentile_sorted(sorted, n, 500000u);
    p90 = percentile_sorted(sorted, n, 900000u);
    p99 = percentile_sorted(sorted, n, 990000u);

    /* Empirical exceedance rate, always available. */
    {
        uint64_t over = 0u;
        for (i = 0; i < n; i++) {
            if (sorted[i] > budget_ns) {
                over++;
            }
        }
        emp = (over * 1000000u) / n;
    }

    if (budget_ns <= p90) {
        /* Budget inside the body of the distribution: empirical rate. */
        return emp;
    }

    /* Peaks-over-threshold: fit an exponential tail above p90.
     *
     * The scale is the mean excess, floored by half the p50..p99 spread.
     * Without that floor a tightly bounded distribution (a good RTOS)
     * produces a mean excess of a few hundred ns and drives the estimate
     * to zero for any budget above it, which would make the predictor blind
     * to the difference between "plenty of headroom" and "some headroom". */
    for (i = 0; i < n; i++) {
        if (sorted[i] > p90) {
            if (first_excess == n) {
                first_excess = i;
            }
            excess_sum += sorted[i] - p90;
            excess_n++;
        }
    }
    if (excess_n == 0u || first_excess == n) {
        return emp;
    }

    base_n = (uint64_t)(n - first_excess); /* samples above threshold */
    {
        double mean_excess = (double)excess_sum / (double)excess_n;
        double spread = 0.5 * (double)(p99 - p50);
        double scale = mean_excess > spread ? mean_excess : spread;
        double tail, ppm;

        if (scale < 1000.0) {
            scale = 1000.0; /* never divide by a vanishing scale */
        }
        tail = ((double)base_n / (double)n) *
               exp(-((double)(budget_ns - p90)) / scale);
        ppm = tail * 1000000.0;
        if (ppm >= 1000000.0) {
            return 1000000u;
        }
        est = (ppm <= 0.0) ? 0u : (uint64_t)(ppm + 0.5);
    }
    return (est > emp) ? est : emp;
}

uint64_t sl_stats_risk_ppm(sl_stats_t *s, uint64_t budget_ns, uint64_t *scratch)
{
    size_t   n, nw;
    uint64_t r_all, r_win;

    n = snapshot(s, scratch, SL_STATS_RING);
    if (n == 0u) {
        return 0u;
    }
    sort_u64(scratch, n);
    r_all = risk_window(scratch, n, budget_ns);

    nw = snapshot(s, scratch, SL_STATS_WINDOW);
    sort_u64(scratch, nw);
    r_win = risk_window(scratch, nw, budget_ns);

    return (r_win > r_all) ? r_win : r_all;
}
