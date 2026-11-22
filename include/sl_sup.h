/*
 * sl_sup.h - the deadline supervisor: pure logic, no OS calls.
 *
 * Everything the supervisor needs from the outside world is a clock
 * reading, a timer arm/disarm pair and event callbacks, so the whole
 * state machine is unit-testable on any host with a fake clock.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL_SUP_H
#define SL_SUP_H

#include "sentinel.h"
#include "sl_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t risk_watch_ppm;    /* enter WATCH above this predicted risk   */
    uint32_t risk_degrade_ppm;  /* enter DEGRADED above this risk          */
    uint32_t stop_after_misses; /* critical misses before SAFE_STOP        */
    uint64_t recover_after_ns;  /* clean operation needed to step back     */
    uint64_t miss_window_ns;    /* sliding window used for escalation      */
} sl_sup_config_t;

void sl_sup_config_defaults(sl_sup_config_t *c);

typedef struct {
    /* OS hooks, supplied by the daemon (real) or the tests (fake). */
    void (*arm)(void *ctx, int slot, uint64_t abs_deadline_ns);
    void (*disarm)(void *ctx, int slot);
    void (*on_mode)(void *ctx, int from, int to, uint16_t reason, uint32_t task);
    void (*log_job)(void *ctx, uint32_t task, uint32_t seq,
                    uint64_t exec_ns, uint64_t budget_ns);
    void (*log_miss)(void *ctx, uint32_t task, uint32_t seq,
                     uint64_t overrun_ns, uint64_t exec_ns);
    void (*log_ctl)(void *ctx, uint8_t what, uint32_t task, uint32_t arg);
    void *ctx;
} sl_sup_ops_t;

typedef struct {
    sl_policy_t policy;
    sl_stats_t  stats;
    uint64_t    jobs;
    uint64_t    misses;
    uint64_t    in_flight;
    uint32_t    run_seq;
    uint64_t    start_ns;
    uint64_t    exec_last_ns;
    uint64_t    slack_last_ns;
    int64_t     slack_worst_ns;
    uint32_t    seq_counter;
    uint32_t    clean_jobs;      /* consecutive jobs without a miss        */
    uint32_t    risk_ppm;        /* last predicted overrun risk            */
    uint64_t    last_miss_ns;
} sl_sup_task_t;

typedef struct {
    sl_sup_ops_t     ops;
    sl_sup_config_t  cfg;
    sl_mode_t        mode;
    uint64_t         mode_since_ns;
    uint64_t         started_ns;
    uint64_t         transitions;
    uint64_t         misses_total;      /* critical misses only   */
    uint64_t         events_lost;       /* seq gaps seen in pulses */
    uint64_t         jobs_total;
    uint64_t         window_start_ns;   /* escalation window      */
    uint32_t         window_count;
    uint32_t         policy_violations;
    uint32_t         n_tasks;
    sl_sup_task_t    tasks[SL_MAX_TASKS];
    uint64_t         scratch[SL_STATS_RING];
} sl_sup_t;

void sl_sup_init(sl_sup_t *s, const sl_sup_ops_t *ops,
                 const sl_sup_config_t *cfg, uint64_t now_ns);

/* Returns task index, or -1 when the table is full / policy invalid. */
int  sl_sup_register(sl_sup_t *s, const sl_policy_t *p);

/* A job begins: arm the deadline timer, return the action for this cycle. */
sl_action_t sl_sup_job_start(sl_sup_t *s, uint32_t task, uint32_t seq,
                             uint64_t now_ns);

/* A job ends. exec_ns <= 0 means "use now - start". */
int  sl_sup_job_done(sl_sup_t *s, uint32_t task, uint32_t seq,
                     uint64_t exec_ns, uint64_t now_ns);

/* The deadline timer for `slot` fired: the job overran its budget. */
void sl_sup_timer_expired(sl_sup_t *s, int slot, uint64_t now_ns);

/* Operator actions. */
int  sl_sup_reset(sl_sup_t *s, uint32_t task, uint64_t now_ns);
int  sl_sup_force_mode(sl_sup_t *s, int mode, uint64_t now_ns);

/* Snapshot for devctl / read(). */
void sl_sup_status(sl_sup_t *s, sl_status_t *out, uint64_t now_ns);

/* Current action implied by the mode. */
sl_action_t sl_sup_action(const sl_sup_t *s);

#ifdef __cplusplus
}
#endif
#endif /* SL_SUP_H */
