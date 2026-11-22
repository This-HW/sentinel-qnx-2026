/*
 * sl_log.c - record ring, writer thread and reader for the event log.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_log.h"
#include "sl_plat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One ring slot: header, payload, CRC. */
typedef struct {
    sl_rec_t hdr;
    uint8_t  payload[SL_LOG_MAX_PAYLOAD];
    uint32_t crc;
} sl_slot_t;

struct sl_log {
    char       path[256];
    FILE      *f;
    uint32_t   decimation;   /* write 1 of every N job records */
    pthread_t  tid;
    int        started;
    int        stop;

    sl_slot_t  ring[SL_LOG_RING];
    uint32_t   prod;         /* written by producer only  */
    uint32_t   cons;         /* written by consumer only  */
    uint64_t   drops;
    uint64_t   written;
    uint64_t   job_count;
    uint32_t   seq;
};

/* ------------------------------------------------------------------ */
uint32_t sl_crc32(const void *data, size_t len, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    size_t i;
    int k;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* ------------------------------------------------------------------ */
static void slot_finish(sl_slot_t *s, uint16_t type, uint64_t t_ns,
                        const void *payload, size_t len, uint32_t seq)
{
    s->hdr.magic = SL_LOG_MAGIC;
    s->hdr.type  = type;
    s->hdr.len   = (uint16_t)len;
    s->hdr.seq   = seq;
    s->hdr.t_ns  = t_ns;
    if (len > 0u && payload != NULL) {
        memcpy(s->payload, payload, len);
    }
    s->crc = sl_crc32(&s->hdr, sizeof(s->hdr), 0u);
    s->crc = sl_crc32(s->payload, len, s->crc);
}

static int push(sl_log_t *l, uint16_t type, uint64_t t_ns,
                const void *payload, size_t len)
{
    uint32_t prod = __atomic_load_n(&l->prod, __ATOMIC_RELAXED);
    uint32_t next = (prod + 1u) % SL_LOG_RING;
    sl_slot_t *s;

    if (len > SL_LOG_MAX_PAYLOAD) {
        return -1;
    }
    if (next == __atomic_load_n(&l->cons, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&l->drops, 1u, __ATOMIC_RELAXED);
        return -1;
    }
    s = &l->ring[prod];
    slot_finish(s, type, t_ns, payload, len, l->seq++);
    __atomic_store_n(&l->prod, next, __ATOMIC_RELEASE);
    return 0;
}

static void writer_drain(sl_log_t *l)
{
    uint32_t cons = l->cons;
    uint32_t prod = __atomic_load_n(&l->prod, __ATOMIC_ACQUIRE);

    while (cons != prod) {
        sl_slot_t *s = &l->ring[cons];
        fwrite(&s->hdr, sizeof(s->hdr), 1, l->f);
        if (s->hdr.len) {
            fwrite(s->payload, s->hdr.len, 1, l->f);
        }
        fwrite(&s->crc, sizeof(s->crc), 1, l->f);
        l->written++;
        cons = (cons + 1u) % SL_LOG_RING;
    }
    l->cons = cons;
    fflush(l->f);
}

static void *writer_main(void *arg)
{
    sl_log_t *l = (sl_log_t *)arg;

    for (;;) {
        int stopping = __atomic_load_n(&l->stop, __ATOMIC_ACQUIRE);

        writer_drain(l);
        if (stopping) {
            /* Final drain done: nothing can be produced any more. */
            if (l->cons == __atomic_load_n(&l->prod, __ATOMIC_ACQUIRE)) {
                break;
            }
            continue;
        }
        /* 2 ms of idle: enough to batch, short enough that a crash
         * loses almost nothing. */
        {
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
sl_log_t *sl_log_create(const char *path, uint32_t decimation)
{
    sl_log_t *l = (sl_log_t *)calloc(1, sizeof(*l));
    if (l == NULL) {
        return NULL;
    }
    snprintf(l->path, sizeof(l->path), "%s", path);
    l->decimation = decimation ? decimation : 1u;
    return l;
}

int sl_log_start(sl_log_t *l)
{
    if (l->started) {
        return 0;
    }
    l->f = fopen(l->path, "wb");
    if (l->f == NULL) {
        return -1;
    }
    if (pthread_create(&l->tid, NULL, writer_main, l) != 0) {
        fclose(l->f);
        l->f = NULL;
        return -1;
    }
    l->started = 1;
    return 0;
}

int sl_log_set_thread_priority(sl_log_t *l, int prio)
{
    struct sched_param sp;

    if (l == NULL || !l->started) {
        return -1;
    }
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    return pthread_setschedparam(l->tid, SCHED_FIFO, &sp);
}

void sl_log_stop(sl_log_t *l)
{
    if (l == NULL) {
        return;
    }
    if (l->started) {
        __atomic_store_n(&l->stop, 1, __ATOMIC_RELEASE);
        pthread_join(l->tid, NULL);
        fflush(l->f);
        fclose(l->f);
        l->f = NULL;
        l->started = 0;
    }
    free(l);
}

void sl_log_job(sl_log_t *l, uint32_t task, uint32_t seq,
                uint64_t exec_ns, uint64_t budget_ns)
{
    sl_ev_job_t ev;
    if (l == NULL || !l->started) {
        return;
    }
    if ((l->job_count++ % l->decimation) != 0u) {
        return;
    }
    ev.task = task;
    ev.seq = seq;
    ev.exec_ns = exec_ns;
    ev.budget_ns = budget_ns;
    push(l, SL_EV_JOB, sl_now_ns(), &ev, sizeof(ev));
}

void sl_log_miss(sl_log_t *l, uint32_t task, uint32_t seq,
                 uint64_t overrun_ns, uint64_t exec_ns)
{
    sl_ev_miss_t ev;
    if (l == NULL || !l->started) {
        return;
    }
    ev.task = task;
    ev.seq = seq;
    ev.overrun_ns = overrun_ns;
    ev.exec_ns = exec_ns;
    push(l, SL_EV_MISS, sl_now_ns(), &ev, sizeof(ev));
}

void sl_log_mode(sl_log_t *l, int from, int to, uint16_t reason, uint32_t task)
{
    sl_ev_mode_t ev;
    if (l == NULL || !l->started) {
        return;
    }
    ev.from = (uint8_t)from;
    ev.to = (uint8_t)to;
    ev.reason = reason;
    ev.task = task;
    push(l, SL_EV_MODE, sl_now_ns(), &ev, sizeof(ev));
}

void sl_log_ctl(sl_log_t *l, uint8_t what, uint32_t task, uint32_t arg)
{
    sl_ev_ctl_t ev;
    if (l == NULL || !l->started) {
        return;
    }
    ev.what = what;
    ev.task = task;
    ev.arg = arg;
    push(l, SL_EV_CTL, sl_now_ns(), &ev, sizeof(ev));
}

uint64_t sl_log_drops(const sl_log_t *l)
{
    return l ? l->drops : 0u;
}

const char *sl_log_name(const sl_log_t *l)
{
    return l ? l->path : "";
}

/* ------------------------------------------------------------------ */
int sl_log_reader_open(sl_log_reader_t *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    return r->f ? 0 : -1;
}

int sl_log_reader_next(sl_log_reader_t *r, sl_rec_t *hdr,
                       void *payload, size_t cap, size_t *plen)
{
    for (;;) {
        size_t n;
        uint32_t crc, calc;

        n = fread(hdr, sizeof(*hdr), 1, r->f);
        if (n != 1u) {
            return feof(r->f) ? 0 : -1;
        }
        if (hdr->magic != SL_LOG_MAGIC || hdr->len > SL_LOG_MAX_PAYLOAD) {
            /* Lost framing: stop cleanly, the tail was truncated. */
            return 0;
        }
        if (hdr->len > 0u) {
            if (fread(payload, hdr->len, 1, r->f) != 1u) {
                return feof(r->f) ? 0 : -1;
            }
        }
        if (fread(&crc, sizeof(crc), 1, r->f) != 1u) {
            return feof(r->f) ? 0 : -1;
        }
        calc = sl_crc32(hdr, sizeof(*hdr), 0u);
        calc = sl_crc32(payload, hdr->len, calc);
        r->records++;
        if (calc != crc) {
            r->bad_records++;
            continue;
        }
        if (plen) {
            *plen = hdr->len;
        }
        (void)cap;
        r->bytes += sizeof(*hdr) + hdr->len + sizeof(crc);
        return 1;
    }
}

void sl_log_reader_close(sl_log_reader_t *r)
{
    if (r->f) {
        fclose(r->f);
        r->f = NULL;
    }
}
