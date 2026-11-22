/*
 * sentinel-ctl - operator/CLI client for the supervisor.
 *
 *   sentinel-ctl status             human table
 *   sentinel-ctl status --json      machine readable (for the report tool,
 *                                   a dashboard, or CI assertions)
 *   sentinel-ctl watch 1s           refresh until Ctrl-C
 *   sentinel-ctl reset [--task N]   clear the latch / escalation counters
 *   sentinel-ctl mode SAFE-STOP     operator override
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentinel.h"
#include "sl_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void print_json(const sl_status_t *st)
{
    uint32_t i;
    printf("{\"mode\":\"%s\",\"mode_code\":%u,\"uptime_s\":%.3f,"
           "\"transitions\":%llu,\"jobs\":%llu,\"misses\":%llu,"
           "\"lost\":%llu,\"violations\":%u,\"tasks\":[",
           sl_mode_name((int)st->mode), st->mode, (double)st->uptime_ns / 1e9,
           (unsigned long long)st->transitions,
           (unsigned long long)st->jobs_total,
           (unsigned long long)st->misses_total,
           (unsigned long long)st->events_lost,
           st->policy_violations);
    for (i = 0; i < st->n_tasks; i++) {
        const sl_task_status_t *t = &st->tasks[i];
        printf("%s{\"name\":\"%s\",\"jobs\":%llu,\"misses\":%llu,"
               "\"exec_last_us\":%.3f,\"exec_p99_us\":%.3f,\"exec_wc_us\":%.3f,"
               "\"slack_us\":%.3f,\"slack_worst_us\":%.3f,\"risk_ppm\":%llu,"
               "\"in_flight\":%llu}",
               i ? "," : "", t->name,
               (unsigned long long)t->jobs, (unsigned long long)t->misses,
               (double)t->exec_last_ns / 1000.0,
               (double)t->exec_p99_ns / 1000.0,
               (double)t->exec_wc_ns / 1000.0,
               (double)t->slack_last_ns / 1000.0,
               (double)t->slack_worst_ns / 1000.0,
               (unsigned long long)t->risk_ppm,
               (unsigned long long)t->in_flight);
    }
    printf("]}\n");
}

static int cmd_status(sl_client_t *c, int json)
{
    sl_status_t st;
    size_t len = 0;

    if (sl_client_call(c, SL_DCMD_STATUS, NULL, 0, &st, sizeof(st), &len) != 0) {
        fprintf(stderr, "sentinel-ctl: status request failed\n");
        return 1;
    }
    if (st.magic != SL_MAGIC) {
        fprintf(stderr, "sentinel-ctl: protocol mismatch (magic 0x%08x)\n", st.magic);
        return 1;
    }
    if (json) {
        print_json(&st);
    } else {
        uint32_t i;
        char buf[4096];
        size_t n = 0;
        sl_client_read(c, buf, sizeof(buf), &n);
        printf("%s", buf);
        (void)i;
    }
    fflush(stdout);
    return 0;
}

static void usage(const char *a0)
{
    printf("usage: %s [-p path] status [--json]\n"
           "       %s [-p path] watch <interval_ms>\n"
           "       %s [-p path] reset [--task N]\n"
           "       %s [-p path] mode <INIT|NOMINAL|WATCH|DEGRADED|SAFE-STOP>\n",
           a0, a0, a0, a0);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *cmd;
    sl_client_t *c = NULL;
    int json = 0;
    const char *mode_arg = NULL;
    uint32_t task = 0xffffffffu;
    int i;

    /* options may appear before the command */
    i = 1;
    while (i < argc && argv[i][0] == '-' && strcmp(argv[i], "--json") != 0) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            path = argv[++i];
            i++;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage(argv[0]);
        return 1;
    }
    cmd = argv[i++];
    /* `mode` takes a positional value; everything else takes flags. */
    if (strcmp(cmd, "mode") == 0 && i < argc && argv[i][0] != '-') {
        mode_arg = argv[i++];
    }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--task") == 0 && i + 1 < argc) {
            task = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
    }

    if (sl_client_open(&c, path) != 0) {
        fprintf(stderr, "sentinel-ctl: no supervisor at %s\n", sl_default_path());
        return 1;
    }

    if (strcmp(cmd, "status") == 0) {
        int rc = cmd_status(c, json);
        sl_client_close(c);
        return rc;
    }
    if (strcmp(cmd, "watch") == 0) {
        int ms = (argc > 2) ? atoi(argv[2]) : 500;
        for (;;) {
            printf("\033[H\033[2J");
            cmd_status(c, json);
            usleep((useconds_t)(ms * 1000));
        }
    }
    if (strcmp(cmd, "reset") == 0) {
        if (sl_client_call(c, SL_DCMD_RESET, &task, sizeof(task), NULL, 0, NULL) != 0) {
            fprintf(stderr, "sentinel-ctl: reset failed\n");
            sl_client_close(c);
            return 1;
        }
        printf("reset sent (task %s)\n", task == 0xffffffffu ? "all" : "selected");
        sl_client_close(c);
        return 0;
    }
    if (strcmp(cmd, "mode") == 0) {
        sl_setmode_t m;
        if (mode_arg != NULL) {
            const char *name = mode_arg;
            m.mode = 0xffffffffu;
            for (int k = 0; k <= SL_MODE_SAFE_STOP; k++) {
                if (strcasecmp(name, sl_mode_name(k)) == 0) {
                    m.mode = (uint32_t)k;
                    break;
                }
            }
            if (m.mode == 0xffffffffu) {
                fprintf(stderr, "sentinel-ctl: unknown mode '%s'\n", name);
                sl_client_close(c);
                return 1;
            }
        } else {
            fprintf(stderr, "sentinel-ctl: mode needs a value\n");
            sl_client_close(c);
            return 1;
        }
        m.task = 0xffffffffu;
        if (sl_client_call(c, SL_DCMD_SETMODE, &m, sizeof(m), NULL, 0, NULL) != 0) {
            fprintf(stderr, "sentinel-ctl: setmode failed\n");
            sl_client_close(c);
            return 1;
        }
        printf("mode set to %s\n", sl_mode_name((int)m.mode));
        sl_client_close(c);
        return 0;
    }

    usage(argv[0]);
    sl_client_close(c);
    return 1;
}
