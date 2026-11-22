/*
 * sl_sup.c - deadline supervisor state machine.
 *
 * Design notes (see docs/DESIGN.md for the rationale):
 *   - a job's hard deadline is enforced by a one-shot timer armed at job
 *     start, so a job that never finishes is caught even if the task is
 *     blocked forever (no cooperation from the task is required);
 *   - the mode ladder is NOMINAL -> WATCH -> DEGRADED -> SAFE_STOP with
 *     SAFE_STOP latched until an operator reset;
 *   - WATCH/DEGRADED can be entered *before* a miss happens, from the
 *     execution-time tail model in sl_stats.c. That is the difference
 *     between reporting a deadline miss and preventing it.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_sup.h"

#include <string.h>

void sl_sup_config_defaults(sl_sup_config_t *c)
{
    c->risk_watch_ppm   = 2000u;      /* 0.2% predicted overrun: watch   */
    c->risk_degrade_ppm = 20000u;     /* 2% predicted overrun: shed load */
    c->stop_after_misses = 3u;        /* 3 critical misses: safe stop    */
    c->recover_after_ns = 3000000000ull; /* 3 s clean to step back       */
    c->miss_window_ns   = 5000000000ull; /* escalation window            */
}

static void emit_mode(sl_sup_t *s, int to, uint16_t reason, uint32_t task,
                      uint64_t now_ns)
{
    int from;

    if ((int)s->mode == to) {
        return;
    }
    from = (int)s->mode;
    s->mode = (sl_mode_t)to;
    s->mode_since_ns = now_ns;
    s->transitions++;
    if (s->ops.on_mode) {
        s->ops.on_mode(s->ops.ctx, from, to, reason, task);
    }
}

void sl_sup_init(sl_sup_t *s, const sl_sup_ops_t *ops,
                 const sl_sup_config_t *cfg, uint64_t now_ns)
{
    memset(s, 0, sizeof(*s));
    if (ops) {
        s->ops = *ops;
    }
    if (cfg) {
        s->cfg = *cfg;
    } else {
        sl_sup_config_defaults(&s->cfg);
    }
    s->mode = SL_MODE_NOMINAL;
    s->mode_since_ns = now_ns;
    s->started_ns = now_ns;
    if (s->ops.log_ctl) {
        s->ops.log_ctl(s->ops.ctx, SL_CTL_RESET, 0xffffffffu, 0u);
    }
}

int sl_sup_register(sl_sup_t *s, const sl_policy_t *p)
{
    uint32_t i;
    sl_sup_task_t *t;

    if (p == NULL || p->name[0] == '\0' || p->budget_ns == 0u) {
        return -1;
    }
    /* Re-registering the same name updates the policy in place. */
    for (i = 0; i < s->n_tasks; i++) {
        if (strncmp(s->tasks[i].policy.name, p->name, SL_NAME_MAX) == 0) {
            s->tasks[i].policy = *p;
            if (s->ops.log_ctl) {
                s->ops.log_ctl(s->ops.ctx, SL_CTL_REGISTER, i, 1u);
            }
            return (int)i;
        }
    }
    if (s->n_tasks >= SL_MAX_TASKS) {
        return -1;
    }
    t = &s->tasks[s->n_tasks];
    memset(t, 0, sizeof(*t));
    t->policy = *p;
    t->slack_worst_ns = (int64_t)p->budget_ns;
    sl_stats_reset(&t->stats);
    if (s->ops.log_ctl) {
        s->ops.log_ctl(s->ops.ctx, SL_CTL_REGISTER, s->n_tasks, 0u);
    }
    return (int)s->n_tasks++;
}

sl_action_t sl_sup_action(const sl_sup_t *s)
{
    switch (s->mode) {
    case SL_MODE_SAFE_STOP:
        return SL_ACTION_HOLD;
    case SL_MODE_WATCH:
    case SL_MODE_DEGRADED:
        return SL_ACTION_THROTTLE;
    default:
        return SL_ACTION_RUN;
    }
}

static uint32_t worst_risk_ppm(sl_sup_t *s)
{
    uint32_t i, worst = 0u;

    for (i = 0; i < s->n_tasks; i++) {
        sl_sup_task_t *t = &s->tasks[i];
        uint64_t r;

        if (sl_stats_count(&t->stats) < 8u) {
            continue;
        }
        r = sl_stats_risk_ppm(&t->stats, t->policy.budget_ns, s->scratch);
        if (r > 0xFFFFFFFFull) {
            r = 0xFFFFFFFFull;
        }
        t->risk_ppm = (uint32_t)r;
        if ((uint32_t)r > worst) {
            worst = (uint32_t)r;
        }
    }
    return worst;
}

static int any_recent_miss(sl_sup_t *s, uint64_t now_ns)
{
    uint32_t i;
    for (i = 0; i < s->n_tasks; i++) {
        if (s->tasks[i].last_miss_ns != 0u &&
            now_ns - s->tasks[i].last_miss_ns < s->cfg.recover_after_ns) {
            return 1;
        }
    }
    return 0;
}

static void evaluate(sl_sup_t *s, uint64_t now_ns)
{
    uint32_t worst;

    if (s->mode == SL_MODE_SAFE_STOP) {
        return; /* latched: only an operator reset leaves this state */
    }

    worst = worst_risk_ppm(s);

    if (worst >= s->cfg.risk_degrade_ppm) {
        emit_mode(s, SL_MODE_DEGRADED, SL_RS_RISK_HIGH, 0xffffffffu, now_ns);
        return;
    }
    if (worst >= s->cfg.risk_watch_ppm) {
        if (s->mode == SL_MODE_NOMINAL) {
            emit_mode(s, SL_MODE_WATCH, SL_RS_RISK_HIGH, 0xffffffffu, now_ns);
        }
        return;
    }
    /* Risk is clear. Step back only after a quiet period with no misses. */
    if (s->mode == SL_MODE_WATCH || s->mode == SL_MODE_DEGRADED) {
        if (now_ns - s->mode_since_ns >= s->cfg.recover_after_ns &&
            !any_recent_miss(s, now_ns)) {
            emit_mode(s, SL_MODE_NOMINAL, SL_RS_RECOVERY, 0xffffffffu, now_ns);
        }
    }
}

static void account_miss(sl_sup_t *s, sl_sup_task_t *t, uint32_t task,
                         uint32_t seq, uint64_t exec_ns, uint64_t overrun_ns,
                         uint64_t now_ns)
{
    t->misses++;
    t->clean_jobs = 0u;
    t->last_miss_ns = now_ns;

    if (s->ops.log_miss) {
        s->ops.log_miss(s->ops.ctx, task, seq, overrun_ns, exec_ns);
    }

    if (t->policy.critical) {
        s->misses_total++;
        if (s->window_start_ns == 0u ||
            now_ns - s->window_start_ns > s->cfg.miss_window_ns) {
            s->window_start_ns = now_ns;
            s->window_count = 0u;
        }
        s->window_count++;
        if (s->window_count >= s->cfg.stop_after_misses) {
            emit_mode(s, SL_MODE_SAFE_STOP, SL_RS_CRITICAL_MISS, task, now_ns);
            return;
        }
        if (s->mode == SL_MODE_NOMINAL || s->mode == SL_MODE_WATCH) {
            emit_mode(s, SL_MODE_DEGRADED, SL_RS_CRITICAL_MISS, task, now_ns);
        }
    } else {
        if (s->mode == SL_MODE_NOMINAL) {
            emit_mode(s, SL_MODE_WATCH, SL_RS_CRITICAL_MISS, task, now_ns);
        }
    }
}

sl_action_t sl_sup_job_start(sl_sup_t *s, uint32_t task, uint32_t seq,
                             uint64_t now_ns)
{
    sl_sup_task_t *t;

    if (task >= s->n_tasks) {
        s->policy_violations++;
        return sl_sup_action(s);
    }
    t = &s->tasks[task];

    if (t->in_flight) {
        /* Overlapping jobs: the previous job never reported completion. */
        s->policy_violations++;
        if (s->ops.disarm) {
            s->ops.disarm(s->ops.ctx, (int)task);
        }
    }
    /* Sequence gaps mean events were lost between the task and here. */
    if (t->jobs > 0u && seq != t->run_seq + 1u) {
        uint32_t gap = seq - (t->run_seq + 1u);
        if (gap < 0x100000u) {
            s->events_lost += gap;
        }
    }
    t->run_seq = seq;
    t->start_ns = now_ns;
    t->in_flight = 1u;
    s->jobs_total++;

    if (s->ops.arm) {
        s->ops.arm(s->ops.ctx, (int)task, now_ns + t->policy.budget_ns);
    }
    return sl_sup_action(s);
}

int sl_sup_job_done(sl_sup_t *s, uint32_t task, uint32_t seq,
                    uint64_t exec_ns, uint64_t now_ns)
{
    sl_sup_task_t *t;
    uint64_t exec, budget;
    int64_t  slack;

    if (task >= s->n_tasks) {
        s->policy_violations++;
        return -1;
    }
    t = &s->tasks[task];
    if (!t->in_flight) {
        /* Completion without a start: duplicate or lost start event. */
        s->policy_violations++;
        return -1;
    }
    if (seq != t->run_seq) {
        s->policy_violations++;
        return -1;
    }

    exec = exec_ns ? exec_ns : (now_ns - t->start_ns);
    budget = t->policy.budget_ns;

    t->in_flight = 0u;
    t->jobs++;
    if (s->ops.disarm) {
        s->ops.disarm(s->ops.ctx, (int)task);
    }

    sl_stats_push(&t->stats, exec);
    t->exec_last_ns = exec;
    slack = (int64_t)budget - (int64_t)exec;
    t->slack_last_ns = (slack > 0) ? (uint64_t)slack : 0u;
    if (slack < t->slack_worst_ns) {
        t->slack_worst_ns = slack;
    }
    if (s->ops.log_job) {
        s->ops.log_job(s->ops.ctx, task, seq, exec, budget);
    }

    if (exec > budget) {
        account_miss(s, t, task, seq, exec, exec - budget, now_ns);
    } else {
        if (t->clean_jobs < 0xFFFFFFFFu) {
            t->clean_jobs++;
        }
    }
    evaluate(s, now_ns);
    return 0;
}

void sl_sup_timer_expired(sl_sup_t *s, int slot, uint64_t now_ns)
{
    sl_sup_task_t *t;
    uint64_t overrun;

    if (slot < 0 || (uint32_t)slot >= s->n_tasks) {
        s->policy_violations++;
        return;
    }
    t = &s->tasks[slot];
    if (!t->in_flight) {
        /* Stale timer: the job completed inside its budget. */
        return;
    }
    overrun = (now_ns > t->start_ns + t->policy.budget_ns)
                  ? (now_ns - (t->start_ns + t->policy.budget_ns))
                  : 0u;
    t->in_flight = 0u;                  /* the job is declared dead */
    t->jobs++;
    if (s->ops.disarm) {
        s->ops.disarm(s->ops.ctx, slot);
    }
    sl_stats_push(&t->stats, t->policy.budget_ns + overrun);
    t->exec_last_ns = t->policy.budget_ns + overrun;
    t->slack_last_ns = 0u;
    if (-(int64_t)overrun < t->slack_worst_ns) {
        t->slack_worst_ns = -(int64_t)overrun;
    }
    account_miss(s, t, (uint32_t)slot, t->run_seq, t->exec_last_ns,
                 overrun, now_ns);
}

int sl_sup_reset(sl_sup_t *s, uint32_t task, uint64_t now_ns)
{
    uint32_t i;
    int changed = 0;

    if (task == 0xffffffffu) {
        for (i = 0; i < s->n_tasks; i++) {
            s->tasks[i].clean_jobs = 0u;
            s->tasks[i].last_miss_ns = 0u;
            s->tasks[i].in_flight = 0u;
            if (s->ops.disarm) {
                s->ops.disarm(s->ops.ctx, (int)i);
            }
        }
        s->misses_total = 0u;
        s->window_count = 0u;
        s->window_start_ns = 0u;
        if (s->mode == SL_MODE_SAFE_STOP || s->mode == SL_MODE_DEGRADED) {
            emit_mode(s, SL_MODE_NOMINAL, SL_RS_OPERATOR, 0xffffffffu, now_ns);
            changed = 1;
        }
    } else if (task < s->n_tasks) {
        s->tasks[task].clean_jobs = 0u;
        s->tasks[task].last_miss_ns = 0u;
        changed = 1;
    } else {
        return -1;
    }
    if (s->ops.log_ctl) {
        s->ops.log_ctl(s->ops.ctx, SL_CTL_RESET, task, (uint32_t)changed);
    }
    return 0;
}

int sl_sup_force_mode(sl_sup_t *s, int mode, uint64_t now_ns)
{
    if (mode < SL_MODE_NOMINAL || mode > SL_MODE_SAFE_STOP) {
        return -1;
    }
    emit_mode(s, mode, SL_RS_OPERATOR, 0xffffffffu, now_ns);
    if (s->ops.log_ctl) {
        s->ops.log_ctl(s->ops.ctx, SL_CTL_SETMODE, 0xffffffffu, (uint32_t)mode);
    }
    return 0;
}

void sl_sup_status(sl_sup_t *s, sl_status_t *out, uint64_t now_ns)
{
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->magic = SL_MAGIC;
    out->version = (SL_VERSION_MAJOR << 16) | SL_VERSION_MINOR;
    out->mode = (uint32_t)s->mode;
    out->n_tasks = s->n_tasks;
    out->uptime_ns = now_ns - s->started_ns;
    out->mode_since_ns = (now_ns > s->mode_since_ns) ? now_ns - s->mode_since_ns : 0u;
    out->transitions = s->transitions;
    out->misses_total = s->misses_total;
    out->events_lost = s->events_lost;
    out->jobs_total = s->jobs_total;
    out->policy_violations = s->policy_violations;

    for (i = 0; i < s->n_tasks; i++) {
        sl_sup_task_t *t = &s->tasks[i];
        sl_task_status_t *o = &out->tasks[i];
        memcpy(o->name, t->policy.name, SL_NAME_MAX);
        o->registered = 1u;
        o->jobs = t->jobs;
        o->misses = t->misses;
        o->exec_last_ns = t->exec_last_ns;
        o->exec_wc_ns = t->stats.wcet;
        o->exec_p99_ns = sl_stats_count(&t->stats)
                             ? sl_stats_percentile(&t->stats, 990000u, s->scratch)
                             : 0u;
        o->slack_last_ns = t->slack_last_ns;
        o->slack_worst_ns = t->slack_worst_ns;
        o->risk_ppm = t->risk_ppm;
        o->in_flight = t->in_flight;
    }
}

const char *sl_mode_name(int mode)
{
    switch (mode) {
    case SL_MODE_INIT:      return "INIT";
    case SL_MODE_NOMINAL:   return "NOMINAL";
    case SL_MODE_WATCH:     return "WATCH";
    case SL_MODE_DEGRADED:  return "DEGRADED";
    case SL_MODE_SAFE_STOP: return "SAFE-STOP";
    default:                return "UNKNOWN";
    }
}

const char *sl_mode_desc(int mode)
{
    switch (mode) {
    case SL_MODE_INIT:      return "starting up";
    case SL_MODE_NOMINAL:   return "all deadlines inside budget";
    case SL_MODE_WATCH:     return "predicted overrun risk rising";
    case SL_MODE_DEGRADED:  return "shedding optional work to protect deadlines";
    case SL_MODE_SAFE_STOP: return "actuator held at safe state, latched";
    default:                return "";
    }
}
