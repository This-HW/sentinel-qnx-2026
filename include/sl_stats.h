/*
 * sl_stats.h - execution-time statistics and deadline-risk estimation.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL_STATS_H
#define SL_STATS_H

#include <stdint.h>
#include <stddef.h>

#define SL_STATS_RING 1024

typedef struct {
    uint64_t ring[SL_STATS_RING];
    uint32_t head;      /* next write position                       */
    uint32_t count;     /* valid samples in ring (<= SL_STATS_RING)  */
    uint64_t total;     /* samples ever pushed                       */
    uint64_t wcet;      /* worst observed                            */
    uint64_t sum;       /* running sum, for the mean                 */
    uint64_t sumsq;     /* running sum of squares (scaled, see below)*/
} sl_stats_t;

void     sl_stats_reset(sl_stats_t *s);
void     sl_stats_push(sl_stats_t *s, uint64_t sample_ns);
size_t   sl_stats_count(const sl_stats_t *s);
uint64_t sl_stats_mean(const sl_stats_t *s);

/*
 * Percentile with linear interpolation on the ring contents.
 * pct_ppm is in parts per million: 500000 = p50, 990000 = p99,
 * 999000 = p99.9. Copies into a caller-provided scratch buffer so
 * the caller controls memory; scratch must hold SL_STATS_RING elements.
 */
uint64_t sl_stats_percentile(sl_stats_t *s, uint32_t pct_ppm, uint64_t *scratch);

/*
 * Estimated probability that the *next* sample exceeds `budget_ns`,
 * in parts per million, capped at 1000000.
 *
 * Model (documented in docs/DESIGN.md):
 *   - below the 90th percentile we use the empirical exceedance rate;
 *   - above it we fit an exponential tail (peaks-over-threshold) whose
 *     scale is the mean excess over p90, which is what a bounded jitter
 *     source with occasional stalls looks like;
 *   - two windows are evaluated (whole ring and the most recent 128
 *     samples) and the larger estimate wins, so a sudden regime change
 *     is reflected before the ring is repopulated.
 */
uint64_t sl_stats_risk_ppm(sl_stats_t *s, uint64_t budget_ns, uint64_t *scratch);

/*
 * Standard deviation of the ring, integer arithmetic in ns.
 * Used for the jitter figure in reports.
 */
uint64_t sl_stats_stddev(const sl_stats_t *s);

#endif /* SL_STATS_H */
