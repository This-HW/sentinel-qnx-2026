/*
 * sl_plat.h - the only OS-dependent surface in Sentinel.
 *
 * Two backends implement this interface:
 *   src/plat/plat_qnx.c    QNX 8.0   : resource manager + native IPC + pulses
 *   src/plat/plat_posix.c  POSIX/Linux: AF_UNIX SEQPACKET + timerfds
 *
 * Everything above this line (the supervisor, statistics, log, all tools)
 * is portable C11 and is exercised on the host by tests/.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL_PLAT_H
#define SL_PLAT_H

#include <stdint.h>
#include <stddef.h>

#define SL_MAX_TIMERS 16

/* Monotonic nanoseconds. Never adjusted, never wraps in practice. */
uint64_t sl_now_ns(void);
void     sl_abs_ns_to_timespec(uint64_t abs_ns, void *ts_out);

typedef struct {
    /* A devctl-style request from a client. Return 0 on success. */
    int (*on_request)(void *ctx, uint32_t dcmd, const void *in, size_t inlen,
                      void *out, size_t outcap, size_t *outlen);
    /* Plain read() of the device: text status snapshot. */
    int (*on_read)(void *ctx, char *buf, size_t cap, size_t *len);
    /* A deadline timer armed with sl_server_arm() expired. */
    void (*on_timer)(void *ctx, int slot);
    /* Called once the server loop is running, before blocking. */
    void (*on_ready)(void *ctx);
} sl_server_ops_t;

typedef struct sl_server sl_server_t;
typedef struct sl_client sl_client_t;

/* ---------------- supervisor side (server) ---------------- */
int  sl_server_open(sl_server_t **out, const char *path,
                    const sl_server_ops_t *ops, void *ctx);
int  sl_server_run(sl_server_t *s);      /* returns <0 only on fatal error */
void sl_server_step(sl_server_t *s);     /* one iteration, for tests      */
void sl_server_stop(sl_server_t *s);
void sl_server_close(sl_server_t *s);

/* One-shot absolute deadline per slot; arming an armed slot re-arms it. */
int  sl_server_arm(sl_server_t *s, int slot, uint64_t abs_deadline_ns);
int  sl_server_disarm(sl_server_t *s, int slot);

/* Raise this process's scheduling priority. Returns the achieved priority,
 * or a negative errno when the platform refuses (unprivileged host). */
int  sl_raise_priority(int prio);

/* ---------------- client side ---------------- */
int  sl_client_open(sl_client_t **out, const char *path);
int  sl_client_call(sl_client_t *c, uint32_t dcmd,
                    const void *in, size_t inlen,
                    void *out, size_t outcap, size_t *outlen);
int  sl_client_read(sl_client_t *c, char *buf, size_t cap, size_t *len);
void sl_client_close(sl_client_t *c);

/* Default rendezvous path: QNX "/dev/sentinel", host "/tmp/sentinel.sock". */
const char *sl_default_path(void);

#endif /* SL_PLAT_H */
