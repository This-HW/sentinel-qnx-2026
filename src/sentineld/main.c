/*
 * sentineld - the deadline supervisor daemon.
 *
 * One process, one thread, no locks: a devctl-style request/reply server
 * (on QNX a resource manager at /dev/sentinel) plus the supervisor state
 * machine driven by deadline timer pulses. Logging is the only other
 * thread in the process and it runs at the lowest priority.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentinel.h"
#include "sl_config.h"
#include "sl_log.h"
#include "sl_plat.h"
#include "sl_sup.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    sl_sup_t       sup;
    sl_log_t      *log;
    sl_server_t   *srv;
    uint64_t       started_ns;
    int            verbose;
    const char    *path;
    uint64_t       miss_print_window_ns;
    uint32_t       miss_printed;
} app_t;

static app_t g_app;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------------- supervisor callbacks ---------------- */
static void cb_arm(void *ctx, int slot, uint64_t abs_deadline_ns)
{
    (void)ctx;
    if (sl_server_arm(g_app.srv, slot, abs_deadline_ns) != 0 && g_app.verbose) {
        fprintf(stderr, "sentineld: arm failed for slot %d\n", slot);
    }
}

static void cb_disarm(void *ctx, int slot)
{
    (void)ctx;
    sl_server_disarm(g_app.srv, slot);
}

static void cb_mode(void *ctx, int from, int to, uint16_t reason, uint32_t task)
{
    uint64_t now = sl_now_ns();
    (void)ctx;
    printf("[%8.3f] MODE %s -> %s  (%s, task %s)\n",
           (double)(now - g_app.started_ns) / 1e9,
           sl_mode_name(from), sl_mode_name(to),
           reason == SL_RS_CRITICAL_MISS ? "critical deadline miss"
           : reason == SL_RS_RISK_HIGH   ? "predicted overrun risk"
           : reason == SL_RS_RECOVERY    ? "recovered"
           : reason == SL_RS_OPERATOR    ? "operator"
                                         : "startup",
           task == 0xffffffffu ? "-"
           : (task < g_app.sup.n_tasks ? g_app.sup.tasks[task].policy.name
                                       : "?"));
    fflush(stdout);
    sl_log_mode(g_app.log, from, to, reason, task);
}

static void cb_log_job(void *ctx, uint32_t task, uint32_t seq,
                       uint64_t exec_ns, uint64_t budget_ns)
{
    (void)ctx;
    sl_log_job(g_app.log, task, seq, exec_ns, budget_ns);
}

static void cb_log_miss(void *ctx, uint32_t task, uint32_t seq,
                        uint64_t overrun_ns, uint64_t exec_ns)
{
    uint64_t now = sl_now_ns();
    (void)ctx;
    sl_log_miss(g_app.log, task, seq, overrun_ns, exec_ns);
    /* The log records every miss; the console is rate limited so that a
     * burst of faults cannot itself become a load source. */
    if (now - g_app.miss_print_window_ns > 1000000000ull) {
        g_app.miss_print_window_ns = now;
        g_app.miss_printed = 0u;
    }
    if (g_app.miss_printed >= 8u) {
        g_app.miss_printed++;
        if (g_app.miss_printed == 9u) {
            printf("[%8.3f] ... further misses suppressed on the console "
                   "(all of them are in the event log)\n",
                   (double)(now - g_app.started_ns) / 1e9);
            fflush(stdout);
        }
        return;
    }
    g_app.miss_printed++;
    printf("[%8.3f] MISS task=%s seq=%u exec=%.3fms overrun=%.3fms\n",
           (double)(now - g_app.started_ns) / 1e9,
           task < g_app.sup.n_tasks ? g_app.sup.tasks[task].policy.name : "?",
           seq, (double)exec_ns / 1e6, (double)overrun_ns / 1e6);
    fflush(stdout);
}

static void cb_log_ctl(void *ctx, uint8_t what, uint32_t task, uint32_t arg)
{
    (void)ctx;
    sl_log_ctl(g_app.log, what, task, arg);
}

/* ---------------- status rendering ---------------- */
static int render_text(char *buf, size_t cap, size_t *len)
{
    char     tmp[4096];
    size_t   n = 0;
    uint32_t i;
    sl_status_t st;

    sl_sup_status(&g_app.sup, &st, sl_now_ns());
    n += (size_t)snprintf(tmp + n, sizeof(tmp) - n,
                          "SENTINEL %s  mode=%s (%s)\n"
                          "uptime=%.3fs  transitions=%llu  jobs=%llu  "
                          "misses=%llu  lost=%llu  violations=%u\n",
                          SL_VERSION_STR, sl_mode_name((int)st.mode),
                          sl_mode_desc((int)st.mode),
                          (double)st.uptime_ns / 1e9,
                          (unsigned long long)st.transitions,
                          (unsigned long long)st.jobs_total,
                          (unsigned long long)st.misses_total,
                          (unsigned long long)st.events_lost,
                          st.policy_violations);
    n += (size_t)snprintf(tmp + n, sizeof(tmp) - n,
                          "%-10s %8s %6s %10s %10s %10s %8s %10s\n",
                          "task", "jobs", "miss", "last_us", "p99_us",
                          "wc_us", "risk", "slack_us");
    for (i = 0; i < st.n_tasks && n < sizeof(tmp) - 200u; i++) {
        sl_task_status_t *t = &st.tasks[i];
        n += (size_t)snprintf(tmp + n, sizeof(tmp) - n,
                              "%-10s %8llu %6llu %10.1f %10.1f %10.1f %7.3f%% %10.1f\n",
                              t->name,
                              (unsigned long long)t->jobs,
                              (unsigned long long)t->misses,
                              (double)t->exec_last_ns / 1000.0,
                              (double)t->exec_p99_ns / 1000.0,
                              (double)t->exec_wc_ns / 1000.0,
                              (double)t->risk_ppm / 10000.0,
                              (double)t->slack_last_ns / 1000.0);
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, tmp, n);
    buf[n] = '\0';
    if (len) {
        *len = n;
    }
    return 0;
}

/* ---------------- request handling ---------------- */
static int on_request(void *ctx, uint32_t dcmd,
                      const void *in, size_t inlen,
                      void *out, size_t outcap, size_t *outlen)
{
    uint64_t now = sl_now_ns();
    (void)ctx;
    *outlen = 0;

    switch (dcmd) {
    case SL_DCMD_STATUS:
        if (outcap < sizeof(sl_status_t)) {
            return -1;
        }
        sl_sup_status(&g_app.sup, (sl_status_t *)out, now);
        *outlen = sizeof(sl_status_t);
        return 0;

    case SL_DCMD_REGISTER: {
        sl_policy_t p;
        int id;
        if (inlen < sizeof(p)) {
            return -1;
        }
        memcpy(&p, in, sizeof(p));
        id = sl_sup_register(&g_app.sup, &p);
        if (id < 0) {
            return -2;
        }
        if (outcap >= sizeof(int)) {
            memcpy(out, &id, sizeof(int));
            *outlen = sizeof(int);
        }
        return 0;
    }

    case SL_DCMD_JOBSTART: {
        sl_job_t j;
        sl_go_t  go;
        if (inlen < sizeof(j)) {
            return -1;
        }
        memcpy(&j, in, sizeof(j));
        go.action = (uint32_t)sl_sup_job_start(&g_app.sup, j.task, j.seq, now);
        go.mode = (uint32_t)g_app.sup.mode;
        if (outcap >= sizeof(go)) {
            memcpy(out, &go, sizeof(go));
            *outlen = sizeof(go);
        }
        return 0;
    }

    case SL_DCMD_JOBDONE: {
        sl_job_t j;
        if (inlen < sizeof(j)) {
            return -1;
        }
        memcpy(&j, in, sizeof(j));
        return sl_sup_job_done(&g_app.sup, j.task, j.seq, j.exec_ns, now);
    }

    case SL_DCMD_RESET: {
        uint32_t task = 0xffffffffu;
        if (inlen >= sizeof(uint32_t)) {
            memcpy(&task, in, sizeof(task));
        }
        return sl_sup_reset(&g_app.sup, task, now);
    }

    case SL_DCMD_SETMODE: {
        sl_setmode_t m;
        if (inlen < sizeof(m)) {
            return -1;
        }
        memcpy(&m, in, sizeof(m));
        return sl_sup_force_mode(&g_app.sup, (int)m.mode, now);
    }

    default:
        return -1;
    }
}

static int on_read(void *ctx, char *buf, size_t cap, size_t *len)
{
    (void)ctx;
    return render_text(buf, cap, len);
}

static void on_timer(void *ctx, int slot)
{
    (void)ctx;
    sl_sup_timer_expired(&g_app.sup, slot, sl_now_ns());
}

static void on_ready(void *ctx)
{
    (void)ctx;
    printf("sentineld %s ready: %s (mode %s)\n", SL_VERSION_STR,
           g_app.path ? g_app.path : sl_default_path(),
           sl_mode_name((int)g_app.sup.mode));
    fflush(stdout);
}

/* ---------------- main ---------------- */
static void usage(const char *argv0)
{
    printf("usage: %s [-c config] [-p path] [-l logfile] [-v]\n"
           "  -c  configuration file (default /etc/sentinel.conf)\n"
           "  -p  rendezvous path\n"
           "  -l  event log file (default /tmp/sentinel.slg)\n",
           argv0);
}

int main(int argc, char **argv)
{
    sl_config_t cfg;
    sl_sup_ops_t ops;
    sl_sup_config_t scfg;
    const char *cfg_path = "/etc/sentinel.conf";
    const char *path = NULL;
    const char *log_path = NULL;
    uint32_t prio;
    int opt;

    while ((opt = getopt(argc, argv, "c:p:l:vh")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'p': path = optarg; break;
        case 'l': log_path = optarg; break;
        case 'v': g_app.verbose = 1; break;
        default: usage(argv[0]); return 0;
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    if (sl_config_load(&cfg, cfg_path) != 0 && g_app.verbose) {
        fprintf(stderr, "sentineld: no config at %s, using defaults\n", cfg_path);
    }

    sl_sup_config_defaults(&scfg);
    scfg.risk_watch_ppm    = sl_config_u32(&cfg, "risk_watch_ppm", scfg.risk_watch_ppm);
    scfg.risk_degrade_ppm  = sl_config_u32(&cfg, "risk_degrade_ppm", scfg.risk_degrade_ppm);
    scfg.stop_after_misses = sl_config_u32(&cfg, "stop_after_misses", scfg.stop_after_misses);
    scfg.recover_after_ns  = sl_config_duration_ns(&cfg, "recover_after", scfg.recover_after_ns);
    scfg.miss_window_ns    = sl_config_duration_ns(&cfg, "miss_window", scfg.miss_window_ns);

    prio = sl_config_u32(&cfg, "priority", 30u);
    if (sl_raise_priority((int)prio) < 0) {
        printf("sentineld: running unprivileged (priority %u not granted)\n", prio);
    } else {
        printf("sentineld: priority %u\n", prio);
    }

    if (log_path == NULL) {
        log_path = sl_config_str(&cfg, "log", "/tmp/sentinel.slg");
    }
    g_app.log = sl_log_create(log_path, sl_config_u32(&cfg, "log_decimation", 4u));
    if (g_app.log == NULL || sl_log_start(g_app.log) != 0) {
        fprintf(stderr, "sentineld: cannot open log %s\n", log_path);
        return 1;
    }
    /* The log writer must never outrank a supervised task. */
    (void)sl_log_set_thread_priority(
        g_app.log, (int)sl_config_u32(&cfg, "log_priority", 5u));

    memset(&ops, 0, sizeof(ops));
    ops.arm      = cb_arm;
    ops.disarm   = cb_disarm;
    ops.on_mode  = cb_mode;
    ops.log_job  = cb_log_job;
    ops.log_miss = cb_log_miss;
    ops.log_ctl  = cb_log_ctl;

    g_app.started_ns = sl_now_ns();
    sl_sup_init(&g_app.sup, &ops, &scfg, g_app.started_ns);

    {
        sl_server_ops_t sops;
        memset(&sops, 0, sizeof(sops));
        sops.on_request = on_request;
        sops.on_read    = on_read;
        sops.on_timer   = on_timer;
        sops.on_ready   = on_ready;
        g_app.path = path ? path : sl_config_str(&cfg, "listen", NULL);
        if (sl_server_open(&g_app.srv, g_app.path, &sops, &g_app) != 0) {
            fprintf(stderr, "sentineld: cannot create channel %s\n",
                    path ? path : sl_default_path());
            return 1;
        }
    }

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    while (!g_stop) {
        sl_server_step(g_app.srv);
    }

    printf("sentineld: shutting down\n");
    sl_server_close(g_app.srv);
    sl_log_stop(g_app.log);
    return 0;
}
