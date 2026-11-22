/*
 * test_sup.c - supervisor state machine with a fake clock and fake timers.
 *
 * The supervisor takes all of its OS dependencies as callbacks, so the
 * escalation ladder can be tested deterministically: no sleeps, no
 * threads, no timing flakiness.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_sup.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* ---- fake platform ---- */
typedef struct {
    uint64_t now;
    uint64_t armed[8];
    int      arm_count;
    int      disarm_count;
    int      mode_changes;
    int      last_from, last_to, last_reason;
    uint64_t jobs_logged, misses_logged;
} fake_t;

static void f_arm(void *ctx, int slot, uint64_t deadline)
{
    fake_t *f = (fake_t *)ctx;
    f->armed[slot] = deadline;
    f->arm_count++;
}

static void f_disarm(void *ctx, int slot)
{
    fake_t *f = (fake_t *)ctx;
    f->armed[slot] = 0;
    f->disarm_count++;
}

static void f_mode(void *ctx, int from, int to, uint16_t reason, uint32_t task)
{
    fake_t *f = (fake_t *)ctx;
    (void)task;
    f->mode_changes++;
    f->last_from = from;
    f->last_to = to;
    f->last_reason = reason;
}

static void f_job(void *ctx, uint32_t task, uint32_t seq, uint64_t exec, uint64_t budget)
{
    fake_t *f = (fake_t *)ctx;
    (void)task; (void)seq; (void)exec; (void)budget;
    f->jobs_logged++;
}

static void f_miss(void *ctx, uint32_t task, uint32_t seq, uint64_t overrun, uint64_t exec)
{
    fake_t *f = (fake_t *)ctx;
    (void)task; (void)seq; (void)overrun; (void)exec;
    f->misses_logged++;
}

static fake_t g_fake;

static void setup(sl_sup_t *s, sl_sup_config_t *cfg)
{
    sl_sup_ops_t ops;
    sl_policy_t p;

    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.now = 1000000;

    memset(&ops, 0, sizeof(ops));
    ops.arm = f_arm;
    ops.disarm = f_disarm;
    ops.on_mode = f_mode;
    ops.log_job = f_job;
    ops.log_miss = f_miss;
    ops.ctx = &g_fake;

    sl_sup_config_defaults(cfg);
    sl_sup_init(s, &ops, cfg, g_fake.now);

    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "control");
    p.period_ns = 1000000ull;  /* 1 ms   */
    p.budget_ns = 900000ull;   /* 0.9 ms */
    p.critical = 1;
    CHECK(sl_sup_register(s, &p) == 0, "register");
}

/* Run a job that completes within its budget. */
static void good_job(sl_sup_t *s, uint32_t seq, uint64_t exec_ns)
{
    g_fake.now += 1000000ull;
    (void)sl_sup_job_start(s, 0, seq, g_fake.now);
    g_fake.now += exec_ns;
    (void)sl_sup_job_done(s, 0, seq, exec_ns, g_fake.now);
}

static void test_healthy(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    int i;

    setup(&s, &cfg);
    for (i = 0; i < 200; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    CHECK(s.mode == SL_MODE_NOMINAL, "mode %d after clean run",
          (int)s.mode);
    CHECK(s.misses_total == 0, "misses %llu", (unsigned long long)s.misses_total);
    CHECK(s.tasks[0].jobs == 200, "jobs %llu",
          (unsigned long long)s.tasks[0].jobs);
    CHECK(g_fake.arm_count == 200, "arm count %d", g_fake.arm_count);
    CHECK(g_fake.jobs_logged == 200, "logged %llu",
          (unsigned long long)g_fake.jobs_logged);
    CHECK(s.tasks[0].slack_worst_ns == 600000, "worst slack %lld",
          (long long)s.tasks[0].slack_worst_ns);
}

static void test_single_miss_degrades(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    int i;

    setup(&s, &cfg);
    for (i = 0; i < 20; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    g_fake.now += 1000000ull;
    (void)sl_sup_job_start(&s, 0, 20, g_fake.now);
    g_fake.now += 1500000ull;
    (void)sl_sup_job_done(&s, 0, 20, 1500000u, g_fake.now);

    CHECK(s.mode == SL_MODE_DEGRADED, "mode %d after one miss", (int)s.mode);
    CHECK(g_fake.last_to == SL_MODE_DEGRADED, "transition target %d",
          g_fake.last_to);
    CHECK(g_fake.last_reason == SL_RS_CRITICAL_MISS, "reason %d",
          g_fake.last_reason);
    CHECK(sl_sup_action(&s) == SL_ACTION_THROTTLE, "action not throttled");
    CHECK(s.misses_total == 1, "misses_total %llu",
          (unsigned long long)s.misses_total);
    CHECK(g_fake.misses_logged == 1, "miss logged %llu",
          (unsigned long long)g_fake.misses_logged);
    CHECK(s.tasks[0].slack_worst_ns == -600000, "overrun slack %lld",
          (long long)s.tasks[0].slack_worst_ns);
}

static void test_three_misses_safe_stop_and_latch(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    int i;

    setup(&s, &cfg);
    for (i = 0; i < 40; i++) {
        good_job(&s, (uint32_t)i, 300000u);
        if (i % 10 == 9) {
            good_job(&s, (uint32_t)++i, 1200000u); /* miss */
        }
    }
    CHECK(s.mode == SL_MODE_SAFE_STOP, "mode %d after 3 misses", (int)s.mode);
    CHECK(sl_sup_action(&s) == SL_ACTION_HOLD, "action not hold");

    /* Latched: further good jobs must not clear it. */
    for (i = 100; i < 300; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    CHECK(s.mode == SL_MODE_SAFE_STOP, "SAFE-STOP not latched (mode %d)",
          (int)s.mode);

    /* Operator reset clears the latch. */
    sl_sup_reset(&s, 0xffffffffu, g_fake.now);
    CHECK(s.mode == SL_MODE_NOMINAL, "reset did not clear latch (mode %d)",
          (int)s.mode);
    CHECK(sl_sup_action(&s) == SL_ACTION_RUN, "action not run after reset");
}

static void test_timer_expiry_counts_as_miss(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;

    setup(&s, &cfg);
    g_fake.now += 1000000ull;
    (void)sl_sup_job_start(&s, 0, 0, g_fake.now);
    CHECK(g_fake.armed[0] == g_fake.now + 900000ull, "timer not armed at deadline");

    /* The task never finishes; the supervisor's timer fires. */
    g_fake.now += 900000ull + 250000ull;
    sl_sup_timer_expired(&s, 0, g_fake.now);

    CHECK(s.tasks[0].misses == 1, "timer expiry not counted as miss");
    CHECK(s.mode == SL_MODE_DEGRADED, "mode %d after timer expiry", (int)s.mode);
    CHECK(s.tasks[0].in_flight == 0, "job still marked in flight");
}

static void test_sequence_gap_counts_lost(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;

    setup(&s, &cfg);
    good_job(&s, 0, 300000u);
    good_job(&s, 1, 300000u);
    good_job(&s, 5, 300000u);   /* 2,3,4 lost on the way */
    CHECK(s.events_lost == 3, "events_lost %llu",
          (unsigned long long)s.events_lost);
}

static void test_duplicate_and_unknown(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;

    setup(&s, &cfg);
    g_fake.now += 1000000ull;
    (void)sl_sup_job_start(&s, 0, 0, g_fake.now);
    (void)sl_sup_job_done(&s, 0, 99, 100000ull, g_fake.now); /* wrong seq */
    CHECK(s.policy_violations == 1, "mismatched seq not caught");
    (void)sl_sup_job_done(&s, 0, 0, 100000ull, g_fake.now);
    (void)sl_sup_job_done(&s, 0, 0, 100000ull, g_fake.now);   /* duplicate  */
    CHECK(s.policy_violations == 2, "duplicate completion not caught");
    (void)sl_sup_job_start(&s, 7, 0, g_fake.now);             /* no such task */
    CHECK(s.policy_violations == 3, "unknown task not caught");
}

/* Many samples at 700 us against a 900 us budget must trip WATCH before
 * any actual miss - that is the predictive part of the design. */
static void test_predictive_watch(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    int i;

    setup(&s, &cfg);
    cfg.risk_watch_ppm = 1000;
    cfg.risk_degrade_ppm = 900000; /* only the tail should trip WATCH */
    sl_sup_init(&s, &s.ops, &cfg, g_fake.now);
    {
        sl_policy_t p;
        memset(&p, 0, sizeof(p));
        snprintf(p.name, sizeof(p.name), "control");
        p.period_ns = 1000000ull;
        p.budget_ns = 900000ull;
        p.critical = 1;
        (void)sl_sup_register(&s, &p);
    }

    for (i = 0; i < 300; i++) {
        uint64_t exec = 400000u + (uint64_t)(i % 40) * 8000u; /* up to 712us */
        good_job(&s, (uint32_t)i, exec);
    }
    CHECK(s.tasks[0].misses == 0, "unexpected miss in predictive test");
    CHECK(s.tasks[0].risk_ppm > 0, "no risk estimated for a 700us tail");
    CHECK(s.mode == SL_MODE_WATCH,
          "expected WATCH from prediction, got %d (risk %u ppm)",
          (int)s.mode, s.tasks[0].risk_ppm);
}

static void test_recovery(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    int i;

    setup(&s, &cfg);
    for (i = 0; i < 50; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    g_fake.now += 1000000ull;
    (void)sl_sup_job_start(&s, 0, 50, g_fake.now);
    g_fake.now += 1500000ull;
    (void)sl_sup_job_done(&s, 0, 50, 1500000u, g_fake.now);
    CHECK(s.mode == SL_MODE_DEGRADED, "not degraded");

    /* Clean jobs, still inside the recovery window: stays DEGRADED. */
    for (i = 51; i < 151; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    CHECK(s.mode == SL_MODE_DEGRADED, "recovered too early (mode %d)",
          (int)s.mode);

    /* Advance past recover_after with clean jobs: steps back to NOMINAL. */
    for (i = 151; i < 3400; i++) {
        good_job(&s, (uint32_t)i, 300000u);
    }
    CHECK(s.mode == SL_MODE_NOMINAL, "did not recover (mode %d)", (int)s.mode);
}

static void test_status_snapshot(void)
{
    sl_sup_t s;
    sl_sup_config_t cfg;
    sl_status_t st;
    int i;

    setup(&s, &cfg);
    for (i = 0; i < 10; i++) {
        good_job(&s, (uint32_t)i, 100000u + (uint64_t)i * 10000u);
    }
    sl_sup_status(&s, &st, g_fake.now);
    CHECK(st.magic == SL_MAGIC, "magic");
    CHECK(st.n_tasks == 1, "n_tasks %u", st.n_tasks);
    CHECK(strcmp(st.tasks[0].name, "control") == 0, "task name");
    CHECK(st.tasks[0].jobs == 10, "jobs");
    CHECK(st.tasks[0].exec_wc_ns == 190000u, "wcet %llu",
          (unsigned long long)st.tasks[0].exec_wc_ns);
    CHECK(st.tasks[0].exec_p99_ns > 0, "p99 not computed");
}

int main(void)
{
    test_healthy();
    test_single_miss_degrades();
    test_three_misses_safe_stop_and_latch();
    test_timer_expiry_counts_as_miss();
    test_sequence_gap_counts_lost();
    test_duplicate_and_unknown();
    test_predictive_watch();
    test_recovery();
    test_status_snapshot();

    printf("test_sup: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
