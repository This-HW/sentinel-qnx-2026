/*
 * sentinel.h - wire protocol and data model shared by every Sentinel component.
 *
 * This header is deliberately OS-neutral: it is compiled into QNX 8.0
 * targets, POSIX host simulations and the host-side report tool.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SENTINEL_H
#define SENTINEL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_VERSION_STR   "0.9.0"
#define SL_VERSION_MAJOR 0
#define SL_VERSION_MINOR 9

#define SL_MAX_TASKS 8
#define SL_NAME_MAX  24

/* Magic appears in status replies and log files; catches ABI drift early. */
#define SL_MAGIC 0x534C4E54u /* 'SLNT' */

/* ------------------------------------------------------------------ */
/* Safety modes                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    SL_MODE_INIT      = 0,
    SL_MODE_NOMINAL   = 1,
    SL_MODE_WATCH     = 2, /* measured risk is climbing; no miss yet      */
    SL_MODE_DEGRADED  = 3, /* a critical task missed: shed optional work  */
    SL_MODE_SAFE_STOP = 4  /* envelope broken: actuator safe state, latch */
} sl_mode_t;

const char *sl_mode_name(int mode);
const char *sl_mode_desc(int mode);

/* ------------------------------------------------------------------ */
/* Per-task policy registered by the supervised task                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char     name[SL_NAME_MAX];
    uint64_t period_ns;   /* nominal job period (0 = aperiodic)          */
    uint64_t budget_ns;   /* hard deadline, measured from job start      */
    uint64_t slack_ns;    /* required margin kept free at job end        */
    uint32_t priority;    /* informational: the priority it runs at      */
    uint32_t critical;    /* 1 = a miss escalates the safety mode        */
} sl_policy_t;

/* ------------------------------------------------------------------ */
/* Status snapshot (devctl SL_DCMD_STATUS, and 'sentinel-ctl status')   */
/* ------------------------------------------------------------------ */
typedef struct {
    char     name[SL_NAME_MAX];
    uint32_t registered;
    uint64_t jobs;              /* jobs observed                              */
    uint64_t misses;            /* jobs that crossed budget_ns                */
    uint64_t exec_last_ns;      /* most recent execution time                 */
    uint64_t exec_wc_ns;        /* worst observed execution time              */
    uint64_t exec_p99_ns;       /* 99th percentile execution time             */
    uint64_t slack_last_ns;     /* deadline - exec, for the last job          */
    int64_t  slack_worst_ns;    /* smallest slack observed (negative=overrun) */
    uint64_t risk_ppm;          /* predicted P(next exec > budget), parts per million */
    uint64_t in_flight;         /* jobs started but not finished              */
} sl_task_status_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t mode;
    uint32_t n_tasks;
    uint64_t uptime_ns;
    uint64_t mode_since_ns;
    uint64_t transitions;
    uint64_t misses_total;
    uint64_t events_lost;
    uint64_t jobs_total;
    uint32_t policy_violations; /* pulse carried an unknown/invalid task id */
    uint32_t ack_drops;         /* control loop could not be notified      */
    sl_task_status_t tasks[SL_MAX_TASKS];
} sl_status_t;

/* ------------------------------------------------------------------ */
/* devctl command codes                                                */
/*                                                                     */
/* On QNX these are real __DIO* codes so that devctl() knows which      */
/* direction to move data in; on a POSIX host build the same names map  */
/* to plain numbers carried over the socket. Both sides of a connection */
/* always come from this header, so the two encodings never mix.        */
/* ------------------------------------------------------------------ */
#if defined(__QNXNTO__)
#include <devctl.h>
#define SL_DCMD_STATUS    __DIOF(_DCMD_MISC, 1, sl_status_t)
#define SL_DCMD_REGISTER  __DIOT(_DCMD_MISC, 2, sl_policy_t)
#define SL_DCMD_RESET     __DIOT(_DCMD_MISC, 3, uint32_t)
#define SL_DCMD_SETMODE   __DIOT(_DCMD_MISC, 4, sl_setmode_t)
#define SL_DCMD_JOBSTART  __DIOTF(_DCMD_MISC, 5, sl_job_t)
#define SL_DCMD_JOBDONE   __DIOT(_DCMD_MISC, 6, sl_job_t)
#define SL_DCMD_READ      0  /* QNX clients call read(2) on the device */
#else
#define SL_DCMD_BASE      0x5300
#define SL_DCMD_STATUS    (SL_DCMD_BASE + 1) /* out: sl_status_t             */
#define SL_DCMD_REGISTER  (SL_DCMD_BASE + 2) /* in : sl_policy_t             */
#define SL_DCMD_RESET     (SL_DCMD_BASE + 3) /* in : uint32_t (all = ~0)     */
#define SL_DCMD_SETMODE   (SL_DCMD_BASE + 4) /* in : sl_setmode_t            */
#define SL_DCMD_JOBSTART  (SL_DCMD_BASE + 5) /* in : sl_job_t   out: sl_go_t  */
#define SL_DCMD_JOBDONE   (SL_DCMD_BASE + 6) /* in : sl_job_t                */
#define SL_DCMD_READ      (SL_DCMD_BASE + 9) /* out: text status snapshot    */
#endif

/* ------------------------------------------------------------------ */
/* Pulse codes (QNX: must sit inside _PULSE_CODE_MINAVAIL..MAXAVAIL)    */
/* ------------------------------------------------------------------ */
#define SL_PULSE_JOB    41 /* payload: (task << 24) | (kind << 22) | seq   */
#define SL_PULSE_TIMER  42 /* payload: deadline timer slot index           */
#define SL_PULSE_ACK    43 /* daemon -> control loop: mode changed         */

#define SL_JOB_START 0u
#define SL_JOB_DONE  1u

#define SL_PULSE_MAKE(task, kind, seq) \
    ((((uint32_t)(task) & 0xFFu) << 24) | (((uint32_t)(kind) & 0x3u) << 22) | \
     ((uint32_t)(seq) & 0x3FFFFFu))
#define SL_PULSE_TASK(v) (((uint32_t)(v) >> 24) & 0xFFu)
#define SL_PULSE_KIND(v) (((uint32_t)(v) >> 22) & 0x3u)
#define SL_PULSE_SEQ(v)  ((uint32_t)(v) & 0x3FFFFFu)

/* devctl payload shapes */
typedef struct {
    uint32_t task;   /* 0xffffffff = all */
    uint32_t mode;   /* target mode */
} sl_setmode_t;

/* One control-loop job, as seen by the supervisor. */
typedef struct {
    uint32_t task;
    uint32_t seq;
    uint64_t exec_ns; /* JOBDONE only: measured execution time, 0 = use arrival delta */
} sl_job_t;

/* Reply to JOBSTART: what this cycle is allowed to do. */
typedef struct {
    uint32_t action;  /* sl_action_t */
    uint32_t mode;    /* sl_mode_t, for display */
} sl_go_t;

/* Why a mode transition happened; recorded in the event log. */
enum sl_mode_reason {
    SL_RS_CRITICAL_MISS = 1,
    SL_RS_RISK_HIGH     = 2,
    SL_RS_RISK_CLEAR    = 3,
    SL_RS_OPERATOR      = 4,
    SL_RS_RECOVERY      = 5,
    SL_RS_STARTUP       = 6,
};

/* Operator/tool actions recorded in the event log. */
enum sl_ctl_what {
    SL_CTL_RESET    = 1,
    SL_CTL_SETMODE  = 2,
    SL_CTL_REGISTER = 3,
};

/* ------------------------------------------------------------------ */
/* Controller decisions: what the supervised control loop should do    */
/* ------------------------------------------------------------------ */
typedef enum {
    SL_ACTION_RUN      = 0, /* full control rate, all features            */
    SL_ACTION_THROTTLE = 1, /* reduce period / skip optional stages       */
    SL_ACTION_HOLD     = 2  /* freeze actuator at last safe setpoint      */
} sl_action_t;

#ifdef __cplusplus
}
#endif
#endif /* SENTINEL_H */
