/*
 * sentinel-report - host-side analysis of a Sentinel event log.
 *
 * This tool runs on Windows, macOS or Linux - it never needs QNX. It reads
 * the .slg file produced by sentineld on the target, recomputes the timing
 * statistics from the raw job records, and emits:
 *   - a summary on stdout
 *   - --json <file>  a machine-readable summary
 *   - --html <file>  a self-contained HTML report (no external assets)
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentinel.h"
#include "sl_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint64_t t_ns;
    uint64_t exec_ns;
    uint64_t budget_ns;
} job_rec_t;

typedef struct {
    uint32_t          id;
    job_rec_t        *jobs;
    size_t            njobs;
    size_t            cap;
    uint64_t          misses;
    uint64_t          worst_exec;
    uint64_t          worst_overrun;
    uint64_t          min_exec;
    uint64_t          budget_ns;
} task_rec_t;

typedef struct {
    uint64_t t_ns;
    uint8_t  from, to;
    uint16_t reason;
} mode_rec_t;

typedef struct {
    mode_rec_t *v;
    size_t      n, cap;
} mode_vec_t;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (q == NULL) {
        fprintf(stderr, "sentinel-report: out of memory\n");
        exit(1);
    }
    return q;
}

static task_rec_t *task_get(task_rec_t **tasks, size_t *ntasks, uint32_t id)
{
    size_t i;
    for (i = 0; i < *ntasks; i++) {
        if ((*tasks)[i].id == id) {
            return &(*tasks)[i];
        }
    }
    *tasks = (task_rec_t *)xrealloc(*tasks, (*ntasks + 1) * sizeof(**tasks));
    memset(&(*tasks)[*ntasks], 0, sizeof(**tasks));
    (*tasks)[*ntasks].id = id;
    (*tasks)[*ntasks].min_exec = UINT64_MAX;
    return &(*tasks)[(*ntasks)++];
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static uint64_t pct(uint64_t *sorted, size_t n, double p)
{
    size_t idx;
    if (n == 0) {
        return 0;
    }
    idx = (size_t)(p * (double)(n - 1) + 0.5);
    if (idx >= n) {
        idx = n - 1;
    }
    return sorted[idx];
}

static double pct_ppm_to_us(uint64_t ns)
{
    return (double)ns / 1000.0;
}

int main(int argc, char **argv)
{
    const char *log_path = NULL, *json_path = NULL, *html_path = NULL;
    const char *task_names[SL_MAX_TASKS];
    int opt;
    sl_log_reader_t rd;
    sl_rec_t hdr;
    uint8_t payload[SL_LOG_MAX_PAYLOAD];
    size_t plen = 0;
    task_rec_t *tasks = NULL;
    size_t ntasks = 0;
    mode_vec_t modes = {NULL, 0, 0};
    uint64_t t0 = 0, t1 = 0;
    int have_t0 = 0;

    for (int k = 0; k < SL_MAX_TASKS; k++) {
        task_names[k] = NULL;
    }
    while ((opt = getopt(argc, argv, "j:h:t:")) != -1) {
        switch (opt) {
        case 'j': json_path = optarg; break;
        case 'h': html_path = optarg; break;
        case 't': {
            /* --name 0=control,1=vision  (task ids are assigned at run time) */
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", optarg);
            char *eq = strchr(buf, '=');
            if (eq) {
                *eq = '\0';
                int id = atoi(buf);
                if (id >= 0 && id < SL_MAX_TASKS) {
                    task_names[id] = strdup(eq + 1);
                }
            }
            break;
        }
        default:
            printf("usage: %s <log.slg> [-j out.json] [-h out.html] [-t id=name]\n",
                   argv[0]);
            return 1;
        }
    }
    if (optind < argc) {
        log_path = argv[optind];
    }
    if (log_path == NULL) {
        printf("usage: %s <log.slg> [-j out.json] [-h out.html] [-t id=name]\n",
               argv[0]);
        return 1;
    }

    if (sl_log_reader_open(&rd, log_path) != 0) {
        fprintf(stderr, "sentinel-report: cannot open %s\n", log_path);
        return 1;
    }

    while (sl_log_reader_next(&rd, &hdr, payload, sizeof(payload), &plen) == 1) {
        if (!have_t0) {
            t0 = hdr.t_ns;
            have_t0 = 1;
        }
        t1 = hdr.t_ns;
        switch (hdr.type) {
        case SL_EV_JOB: {
            sl_ev_job_t ev;
            task_rec_t *t;
            if (plen < sizeof(ev)) {
                break;
            }
            memcpy(&ev, payload, sizeof(ev));
            t = task_get(&tasks, &ntasks, ev.task);
            if (t->njobs == t->cap) {
                t->cap = t->cap ? t->cap * 2 : 1024;
                t->jobs = (job_rec_t *)xrealloc(t->jobs, t->cap * sizeof(job_rec_t));
            }
            t->jobs[t->njobs].t_ns = hdr.t_ns;
            t->jobs[t->njobs].exec_ns = ev.exec_ns;
            t->jobs[t->njobs].budget_ns = ev.budget_ns;
            t->njobs++;
            t->budget_ns = ev.budget_ns;
            if (ev.exec_ns > t->worst_exec) {
                t->worst_exec = ev.exec_ns;
            }
            if (ev.exec_ns < t->min_exec) {
                t->min_exec = ev.exec_ns;
            }
            break;
        }
        case SL_EV_MISS: {
            sl_ev_miss_t ev;
            task_rec_t *t;
            if (plen < sizeof(ev)) {
                break;
            }
            memcpy(&ev, payload, sizeof(ev));
            t = task_get(&tasks, &ntasks, ev.task);
            t->misses++;
            if (ev.overrun_ns > t->worst_overrun) {
                t->worst_overrun = ev.overrun_ns;
            }
            break;
        }
        case SL_EV_MODE: {
            sl_ev_mode_t ev;
            if (plen < sizeof(ev)) {
                break;
            }
            memcpy(&ev, payload, sizeof(ev));
            if (modes.n == modes.cap) {
                modes.cap = modes.cap ? modes.cap * 2 : 64;
                modes.v = (mode_rec_t *)xrealloc(modes.v, modes.cap * sizeof(mode_rec_t));
            }
            modes.v[modes.n].t_ns = hdr.t_ns;
            modes.v[modes.n].from = ev.from;
            modes.v[modes.n].to = ev.to;
            modes.v[modes.n].reason = ev.reason;
            modes.n++;
            break;
        }
        default:
            break;
        }
    }
    sl_log_reader_close(&rd);

    if (ntasks == 0) {
        fprintf(stderr, "sentinel-report: no job records in %s\n", log_path);
        return 1;
    }

    /* ---------------- compute ---------------- */
    double duration_s = (double)(t1 - t0) / 1e9;
    uint64_t total_jobs = 0, total_misses = 0, worst_exec = 0;

    for (size_t i = 0; i < ntasks; i++) {
        task_rec_t *t = &tasks[i];
        total_jobs += t->njobs;
        total_misses += t->misses;
        if (t->worst_exec > worst_exec) {
            worst_exec = t->worst_exec;
        }
    }

    uint64_t **sorted_exec = (uint64_t **)xrealloc(NULL, ntasks * sizeof(uint64_t *));
    for (size_t i = 0; i < ntasks; i++) {
        task_rec_t *t = &tasks[i];
        sorted_exec[i] = (uint64_t *)xrealloc(NULL, (t->njobs ? t->njobs : 1) * sizeof(uint64_t));
        for (size_t k = 0; k < t->njobs; k++) {
            sorted_exec[i][k] = t->jobs[k].exec_ns;
        }
        qsort(sorted_exec[i], t->njobs, sizeof(uint64_t), cmp_u64);
    }

    /* ---------------- stdout ---------------- */
    printf("Sentinel run report\n");
    printf("  log             %s\n", log_path);
    printf("  records         %llu (%llu corrupt)\n",
           (unsigned long long)rd.records, (unsigned long long)rd.bad_records);
    printf("  duration        %.3f s\n", duration_s);
    printf("  jobs            %llu\n", (unsigned long long)total_jobs);
    printf("  misses          %llu\n", (unsigned long long)total_misses);
    printf("  mode changes    %zu\n", modes.n);
    printf("\n");
    printf("  %-10s %8s %6s %9s %9s %9s %9s %9s\n",
           "task", "jobs", "miss", "min_us", "p50_us", "p99_us", "p999_us", "wc_us");
    for (size_t i = 0; i < ntasks; i++) {
        task_rec_t *t = &tasks[i];
        const char *name = (t->id < SL_MAX_TASKS && task_names[t->id])
                               ? task_names[t->id] : "?";
        printf("  %-10s %8llu %6llu %9.1f %9.1f %9.1f %9.1f %9.1f\n",
               name, (unsigned long long)t->njobs,
               (unsigned long long)t->misses,
               pct_ppm_to_us(t->njobs ? sorted_exec[i][0] : 0),
               pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.50)),
               pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.99)),
               pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.999)),
               pct_ppm_to_us(t->worst_exec));
    }

    /* ---------------- JSON ---------------- */
    if (json_path) {
        FILE *f = fopen(json_path, "w");
        if (f == NULL) {
            fprintf(stderr, "sentinel-report: cannot write %s\n", json_path);
            return 1;
        }
        fprintf(f, "{\"log\":\"%s\",\"duration_s\":%.3f,\"records\":%llu,"
                   "\"corrupt\":%llu,\"jobs\":%llu,\"misses\":%llu,"
                   "\"mode_changes\":%zu,\"tasks\":[",
                log_path, duration_s, (unsigned long long)rd.records,
                (unsigned long long)rd.bad_records,
                (unsigned long long)total_jobs,
                (unsigned long long)total_misses, modes.n);
        for (size_t i = 0; i < ntasks; i++) {
            task_rec_t *t = &tasks[i];
            const char *name = (t->id < SL_MAX_TASKS && task_names[t->id])
                                   ? task_names[t->id] : "?";
            fprintf(f, "%s{\"name\":\"%s\",\"jobs\":%llu,\"misses\":%llu,"
                       "\"budget_us\":%.3f,\"min_us\":%.3f,\"p50_us\":%.3f,"
                       "\"p99_us\":%.3f,\"p999_us\":%.3f,\"wc_us\":%.3f,"
                       "\"worst_overrun_us\":%.3f}",
                    i ? "," : "", name, (unsigned long long)t->njobs,
                    (unsigned long long)t->misses,
                    (double)t->budget_ns / 1000.0,
                    pct_ppm_to_us(t->njobs ? sorted_exec[i][0] : 0),
                    pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.50)),
                    pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.99)),
                    pct_ppm_to_us(pct(sorted_exec[i], t->njobs, 0.999)),
                    pct_ppm_to_us(t->worst_exec),
                    pct_ppm_to_us(t->worst_overrun));
        }
        fprintf(f, "]}\n");
        fclose(f);
        printf("\n  wrote %s\n", json_path);
    }

    /* ---------------- HTML ---------------- */
    if (html_path) {
        FILE *f = fopen(html_path, "w");
        if (f == NULL) {
            fprintf(stderr, "sentinel-report: cannot write %s\n", html_path);
            return 1;
        }
        fprintf(f,
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<title>Sentinel run report</title><style>"
            "body{background:#0d1117;color:#c9d1d9;font:14px/1.5 ui-monospace,"
            "SFMono-Regular,Menlo,monospace;margin:0;padding:32px}"
            "h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:28px 0 8px;"
            "color:#e6edf3;border-bottom:1px solid #21262d;padding-bottom:6px}"
            ".sub{color:#8b949e;margin-bottom:24px}"
            ".kpi{display:flex;gap:28px;flex-wrap:wrap;margin:16px 0 8px}"
            ".kpi div{background:#161b22;border:1px solid #21262d;border-radius:6px;"
            "padding:10px 16px;min-width:120px}"
            ".kpi b{display:block;font-size:20px;color:#e6edf3}"
            ".kpi span{color:#8b949e;font-size:11px;text-transform:uppercase;"
            "letter-spacing:.06em}table{border-collapse:collapse;margin-top:8px}"
            "td,th{padding:4px 14px 4px 0;text-align:right;font-weight:400}"
            "th{color:#8b949e;border-bottom:1px solid #21262d}"
            "td:first-child,th:first-child{text-align:left;color:#e6edf3}"
            ".bad{color:#f85149}.warn{color:#d29922}.ok{color:#3fb950}"
            "svg{display:block;margin:10px 0}"
            ".legend{color:#8b949e;font-size:12px}"
            "</style></head><body>");
        fprintf(f, "<h1>Sentinel deadline report</h1><div class=\"sub\">%s &middot; "
                   "%.2f s &middot; %llu job records &middot; %llu corrupt records</div>",
                log_path, duration_s, (unsigned long long)rd.records,
                (unsigned long long)rd.bad_records);
        fprintf(f, "<div class=\"kpi\"><div><span>jobs</span><b>%llu</b></div>"
                   "<div><span>deadline misses</span><b class=\"%s\">%llu</b></div>"
                   "<div><span>mode changes</span><b>%zu</b></div>"
                   "<div><span>worst execution</span><b>%.1f us</b></div></div>",
                (unsigned long long)total_jobs,
                total_misses ? "bad" : "ok",
                (unsigned long long)total_misses, modes.n,
                pct_ppm_to_us(worst_exec));

        /* mode timeline */
        if (modes.n > 0 && t1 > t0) {
            int W = 900, H = 46;
            fprintf(f, "<h2>Safety-mode timeline</h2><svg width=\"%d\" height=\"%d\">",
                    W, H);
            double span = (double)(t1 - t0);
            int prev = SL_MODE_NOMINAL;
            uint64_t prev_t = t0;
            for (size_t i = 0; i <= modes.n; i++) {
                uint64_t at = (i < modes.n) ? modes.v[i].t_ns : t1;
                /* This segment runs from prev_t up to the transition at `at`,
                 * so it is drawn in the mode already in effect (the mode from
                 * the previous transition, NOMINAL before the first one). */
                int mode = prev;
                const char *col = mode == SL_MODE_SAFE_STOP ? "#f85149"
                                : mode == SL_MODE_DEGRADED ? "#d29922"
                                : mode == SL_MODE_WATCH ? "#58a6ff" : "#3fb950";
                double x0 = (double)(prev_t - t0) / span * W;
                double w = (double)(at - prev_t) / span * W;
                if (w < 1.0) {
                    w = 1.0;
                }
                fprintf(f, "<rect x=\"%.1f\" y=\"8\" width=\"%.1f\" height=\"22\" "
                           "fill=\"%s\"/>", x0, w, col);
                prev_t = at;
                if (i < modes.n) {
                    prev = modes.v[i].to;
                }
            }
            fprintf(f, "<line x1=\"0\" y1=\"34\" x2=\"%d\" y2=\"34\" "
                       "stroke=\"#21262d\"/></svg>", W);
            fprintf(f, "<div class=\"legend\">green NOMINAL &middot; blue WATCH "
                       "&middot; amber DEGRADED &middot; red SAFE-STOP</div>");
            fprintf(f, "<table><tr><th>t (s)</th><th>transition</th><th>reason</th></tr>");
            for (size_t i = 0; i < modes.n; i++) {
                const char *r = modes.v[i].reason == SL_RS_CRITICAL_MISS ? "critical deadline miss"
                              : modes.v[i].reason == SL_RS_RISK_HIGH ? "predicted overrun risk"
                              : modes.v[i].reason == SL_RS_RECOVERY ? "recovered"
                              : modes.v[i].reason == SL_RS_OPERATOR ? "operator" : "startup";
                fprintf(f, "<tr><td>%.3f</td><td>%s &rarr; %s</td><td>%s</td></tr>",
                        (double)(modes.v[i].t_ns - t0) / 1e9,
                        sl_mode_name(modes.v[i].from), sl_mode_name(modes.v[i].to), r);
            }
            fprintf(f, "</table>");
        }

        /* per-task */
        for (size_t i = 0; i < ntasks; i++) {
            task_rec_t *t = &tasks[i];
            const char *name = (t->id < SL_MAX_TASKS && task_names[t->id])
                                   ? task_names[t->id] : "task";
            uint64_t p50 = pct(sorted_exec[i], t->njobs, 0.50);
            uint64_t p99 = pct(sorted_exec[i], t->njobs, 0.99);
            uint64_t p999 = pct(sorted_exec[i], t->njobs, 0.999);
            uint64_t minv = t->njobs ? sorted_exec[i][0] : 0;

            fprintf(f, "<h2>%s (id %u)</h2>", name, t->id);
            fprintf(f, "<table><tr><th>jobs</th><th>misses</th><th>budget</th>"
                       "<th>min</th><th>p50</th><th>p99</th><th>p99.9</th>"
                       "<th>worst</th></tr>");
            fprintf(f, "<tr><td>%llu</td><td class=\"%s\">%llu</td><td>%.1f us</td>"
                       "<td>%.1f</td><td>%.1f</td><td>%.1f</td><td>%.1f</td>"
                       "<td class=\"%s\">%.1f</td></tr></table>",
                    (unsigned long long)t->njobs,
                    t->misses ? "bad" : "ok",
                    (unsigned long long)t->misses,
                    (double)t->budget_ns / 1000.0,
                    pct_ppm_to_us(minv), pct_ppm_to_us(p50), pct_ppm_to_us(p99),
                    pct_ppm_to_us(p999),
                    t->worst_exec > t->budget_ns ? "bad" : "ok",
                    pct_ppm_to_us(t->worst_exec));

            /* histogram: 48 buckets from 0 to 1.2x budget (or worst) */
            {
                int B = 48, W = 900, H = 120;
                uint64_t top = t->budget_ns * 12 / 10;
                uint64_t *bins = (uint64_t *)xrealloc(NULL, (size_t)B * sizeof(uint64_t));
                uint64_t maxbin = 1;
                memset(bins, 0, (size_t)B * sizeof(uint64_t));
                if (t->worst_exec > top) {
                    top = t->worst_exec;
                }
                if (top == 0) {
                    top = 1;
                }
                for (size_t k = 0; k < t->njobs; k++) {
                    int b = (int)((double)t->jobs[k].exec_ns / (double)top * (B - 1) + 0.5);
                    if (b < 0) b = 0;
                    if (b >= B) b = B - 1;
                    bins[b]++;
                }
                for (int b = 0; b < B; b++) {
                    if (bins[b] > maxbin) maxbin = bins[b];
                }
                fprintf(f, "<h2>Execution time distribution</h2><svg width=\"%d\" height=\"%d\">",
                        W, H);
                double bw = (double)W / B;
                for (int b = 0; b < B; b++) {
                    double h = (double)bins[b] / (double)maxbin * (H - 20);
                    double x = b * bw;
                    double center = (double)(b) / (B - 1) * (double)top;
                    const char *col = center > (double)t->budget_ns ? "#f85149" : "#3fb950";
                    fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" "
                               "fill=\"%s\" opacity=\"0.85\"/>",
                            x, (H - 16) - h, bw - 1.0, h, col);
                }
                {
                    double bx = (double)t->budget_ns / (double)top * W;
                    fprintf(f, "<line x1=\"%.1f\" y1=\"0\" x2=\"%.1f\" y2=\"%d\" "
                               "stroke=\"#f0f6fc\" stroke-dasharray=\"4 3\"/>",
                            bx, bx, H - 16);
                    fprintf(f, "<text x=\"%.1f\" y=\"%d\" fill=\"#8b949e\" font-size=\"10\">"
                               "budget %.0f us</text></svg>",
                            bx + 4, H - 20, (double)t->budget_ns / 1000.0);
                }
                free(bins);
            }
        }
        fprintf(f, "</body></html>\n");
        fclose(f);
        printf("  wrote %s\n", html_path);
    }

    for (size_t i = 0; i < ntasks; i++) {
        free(sorted_exec[i]);
        free(tasks[i].jobs);
    }
    free(sorted_exec);
    free(tasks);
    free(modes.v);
    return 0;
}
