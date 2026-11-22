/*
 * sl_log.h - deterministic, CRC-protected event log.
 *
 * The supervisor writes fixed-size records into a single-producer /
 * single-consumer ring; a low-priority writer thread drains it to disk.
 * The real-time path therefore never touches a file descriptor, never
 * allocates, and never blocks on I/O - it can only lose records (counted
 * in drops) if the disk falls behind.
 *
 * The same file format is read back by the host tool 'sentinel-report',
 * which does not need the target or the QNX toolchain.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL_LOG_H
#define SL_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define SL_LOG_MAGIC    0x534C4C47u /* 'SLLG' */
#define SL_LOG_RING     4096u
#define SL_LOG_MAX_PAYLOAD 64u

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t len;
    uint32_t seq;
    uint64_t t_ns;
} sl_rec_t;

enum sl_ev_type {
    SL_EV_JOB    = 1, /* sl_ev_job_t   */
    SL_EV_MISS   = 2, /* sl_ev_miss_t  */
    SL_EV_MODE   = 3, /* sl_ev_mode_t  */
    SL_EV_CTL    = 4, /* sl_ev_ctl_t   */
};

typedef struct {
    uint32_t task;
    uint32_t seq;
    uint64_t exec_ns;
    uint64_t budget_ns;
} sl_ev_job_t;

typedef struct {
    uint32_t task;
    uint32_t seq;
    uint64_t overrun_ns;
    uint64_t exec_ns;
} sl_ev_miss_t;

typedef struct {
    uint8_t  from;
    uint8_t  to;
    uint16_t reason;  /* sl_mode_reason */
    uint32_t task;    /* task that triggered it, or 0xffffffff */
} sl_ev_mode_t;

typedef struct {
    uint8_t  what;    /* sl_ctl_what */
    uint32_t task;
    uint32_t arg;
} sl_ev_ctl_t;

/* ---------------- writer ---------------- */
typedef struct sl_log sl_log_t;

sl_log_t *sl_log_create(const char *path, uint32_t decimation);
int       sl_log_start(sl_log_t *l);
void      sl_log_stop(sl_log_t *l);   /* flush + join */

/* The writer thread inherits the creating thread's priority; on an RTOS it
 * must be pushed below every supervised task. Returns 0 on success. */
int       sl_log_set_thread_priority(sl_log_t *l, int prio);

void sl_log_job(sl_log_t *l, uint32_t task, uint32_t seq,
                uint64_t exec_ns, uint64_t budget_ns);
void sl_log_miss(sl_log_t *l, uint32_t task, uint32_t seq,
                 uint64_t overrun_ns, uint64_t exec_ns);
void sl_log_mode(sl_log_t *l, int from, int to, uint16_t reason, uint32_t task);
void sl_log_ctl(sl_log_t *l, uint8_t what, uint32_t task, uint32_t arg);
uint64_t sl_log_drops(const sl_log_t *l);
const char *sl_log_name(const sl_log_t *l);

/* ---------------- reader (host tool) ---------------- */
typedef struct {
    FILE    *f;
    uint32_t seq;
    uint64_t records;
    uint64_t bad_records;
    uint64_t bytes;
} sl_log_reader_t;

int sl_log_reader_open(sl_log_reader_t *r, const char *path);
/* 1 = record read, 0 = clean end of file, -1 = read error. Corrupt
 * records are skipped and counted rather than aborting the scan. */
int sl_log_reader_next(sl_log_reader_t *r, sl_rec_t *hdr,
                       void *payload, size_t cap, size_t *plen);
void sl_log_reader_close(sl_log_reader_t *r);

uint32_t sl_crc32(const void *data, size_t len, uint32_t seed);

#endif /* SL_LOG_H */
