/*
 * plat_qnx.c - QNX 8.0 (QNX Everywhere) backend.
 *
 * The supervisor is a resource manager: /dev/sentinel appears in the
 * pathname space, so every supervised task and every operator tool reaches
 * it with plain POSIX calls (open/devctl/read) while the data path stays a
 * native message. Deadline timers are POSIX timers whose expiry is
 * delivered as a *pulse* straight into the resource manager's channel, so
 * the supervisor is woken by the kernel, not by polling.
 *
 * Mapping to the supervisor core:
 *
 *   sl_server_arm()      -> timer_settime(TIMER_ABSTIME) on a SIGEV_PULSE timer
 *   sl_server_disarm()   -> timer_settime() with a zero interval
 *   on_timer()           -> pulse_attach() handler for SL_PULSE_TIMER
 *   on_request()         -> io_devctl() handler
 *   on_read()            -> io_read() handler
 *   sl_client_call()     -> devctl()
 *
 * Only this file is QNX-specific; the supervisor, the statistics, the log
 * and every tool are the same code that runs in the host simulation.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_plat.h"
#include "sentinel.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <time.h>
#include <unistd.h>

struct sl_server {
    dispatch_t         *dpp;
    dispatch_context_t *ctp;
    int                 chid;
    int                 coid;      /* used by our own timers to pulse us  */
    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t      io_funcs;
    iofunc_attr_t          attr;
    sl_server_ops_t        ops;
    void                  *ctx;
    timer_t                timer[SL_MAX_TIMERS];
    int                    timer_created[SL_MAX_TIMERS];
    int                    armed[SL_MAX_TIMERS];
    char                   path[128];
};

struct sl_client {
    int fd;
};

/* A resource manager is single-threaded by design, so one supervisor per
 * process is the intended shape: the handlers reach it through here. */
static sl_server_t *g_srv;

/* ------------------------------------------------------------------ */
int sl_raise_priority(int prio)
{
    struct sched_param sp;
    int rc;

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        return -rc;
    }
    /* Lock the supervisor's memory: no page fault while enforcing a deadline. */
    (void)mlockall(MCL_CURRENT | MCL_FUTURE);
    return prio;
}

const char *sl_default_path(void)
{
    return "/dev/sentinel";
}

/* ---------------- resource manager callbacks ---------------- */
static int rm_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    struct _io_devctl_reply *rep = (struct _io_devctl_reply *)ctp->msg;
    void   *in;
    void   *out;
    size_t  inlen;
    size_t  outlen = 0;
    size_t  outcap;
    int     sts;
    int     rc;

    /* Let the framework handle the system DCMD_ALL_* commands first. */
    if ((sts = iofunc_devctl_default(ctp, msg, ocb)) != _RESMGR_DEFAULT) {
        return sts;
    }

    /* Validate the message against what was actually received: ctp->size
     * is the received length, and a client cannot claim more than that. */
    if (ctp->size < sizeof(msg->i)) {
        return EBADMSG;
    }
    inlen = ctp->size - sizeof(msg->i);
    in    = _IO_INPUT_PAYLOAD(msg);

    out    = _IO_OUTPUT_PAYLOAD(*rep);
    outcap = (ctp->msg_max_size > ctp->offset + sizeof(*rep))
                 ? ctp->msg_max_size - ctp->offset - sizeof(*rep)
                 : 0;

    rc = g_srv->ops.on_request(g_srv->ctx, (uint32_t)msg->i.dcmd,
                               in, inlen, out, outcap, &outlen);
    if (rc != 0) {
        return (rc == -2) ? ENOSPC : EINVAL;
    }
    if (outlen > outcap) {
        return EOVERFLOW;
    }

    memset(rep, 0, sizeof(*rep));
    rep->ret_val = 0;
    SETIOV(ctp->iov, rep, sizeof(*rep) + outlen);
    return _RESMGR_NPARTS(1);
}

static int rm_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    static char snapshot[2048];
    static size_t snapshot_len;
    size_t len, nbytes, nleft;
    int    sts;

    if ((sts = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK) {
        return sts;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }

    /* One snapshot per open sequence: a status read is a consistent view. */
    if (ocb->offset == 0) {
        snapshot_len = 0;
        if (g_srv->ops.on_read) {
            g_srv->ops.on_read(g_srv->ctx, snapshot, sizeof(snapshot),
                               &snapshot_len);
        }
        if (snapshot_len > sizeof(snapshot) - 1u) {
            snapshot_len = sizeof(snapshot) - 1u;
        }
        snapshot[snapshot_len] = '\0';
    }

    len = strlen(snapshot);
    if ((size_t)ocb->offset >= len) {
        return 0;
    }
    nleft = len - (size_t)ocb->offset;
    nbytes = (size_t)msg->i.nbytes;
    if (nbytes == 0u || nbytes > nleft) {
        nbytes = nleft;
    }
    SETIOV(ctp->iov, snapshot + ocb->offset, nbytes);
    ocb->offset += (off_t)nbytes;
    return _RESMGR_NPARTS(1);
}

static int rm_pulse(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    struct _pulse *pulse = (struct _pulse *)ctp->msg;
    int slot;

    (void)flags;
    (void)handle;
    if (code != SL_PULSE_TIMER) {
        return 0;
    }
    slot = pulse->value.sival_int;
    if (slot >= 0 && slot < SL_MAX_TIMERS) {
        g_srv->armed[slot] = 0;
        if (g_srv->ops.on_timer) {
            g_srv->ops.on_timer(g_srv->ctx, slot);
        }
    }
    return 0;
}

/* ---------------- server ---------------- */
int sl_server_open(sl_server_t **out, const char *path,
                   const sl_server_ops_t *ops, void *ctx)
{
    sl_server_t    *s;
    resmgr_attr_t   rattr;
    struct sigevent event;
    int             id;
    int             i;

    s = (sl_server_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return -1;
    }
    s->ops = *ops;
    s->ctx = ctx;
    s->coid = -1;
    snprintf(s->path, sizeof(s->path), "%s", path ? path : sl_default_path());
    g_srv = s;

    /* A public channel of our own, handed to the dispatch layer: this is
     * the channel clients reach through /dev/sentinel and the channel our
     * timer pulses land on. */
    s->chid = ChannelCreate(0);
    if (s->chid == -1) {
        free(s);
        return -1;
    }
    s->dpp = dispatch_create_channel(s->chid, DISPATCH_FLAG_NOLOCK);
    if (s->dpp == NULL) {
        ConnectDetach(s->chid);
        free(s);
        return -1;
    }

    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 1;
    rattr.msg_max_size = 8192;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &s->connect_funcs,
                     _RESMGR_IO_NFUNCS, &s->io_funcs);
    s->io_funcs.devctl = rm_devctl;
    s->io_funcs.read = rm_read;
    iofunc_attr_init(&s->attr, S_IFNAM | 0666, 0, 0);

    id = resmgr_attach(s->dpp, &rattr, s->path, _FTYPE_ANY, 0,
                       &s->connect_funcs, &s->io_funcs, &s->attr);
    if (id == -1) {
        dispatch_destroy(s->dpp);
        free(s);
        return -1;
    }

    s->coid = ConnectAttach(0, 0, s->chid, _NTO_SIDE_CHANNEL, 0);
    if (s->coid == -1) {
        dispatch_destroy(s->dpp);
        free(s);
        return -1;
    }
    if (pulse_attach(s->dpp, 0, SL_PULSE_TIMER, rm_pulse, NULL) == -1) {
        ConnectDetach(s->coid);
        dispatch_destroy(s->dpp);
        free(s);
        return -1;
    }

    /* One deadline timer per supervised task slot. Created up front so the
     * real-time path never has to create an object. */
    for (i = 0; i < SL_MAX_TIMERS; i++) {
        SIGEV_PULSE_INT_INIT(&event, s->coid, SIGEV_PULSE_PRIO_INHERIT,
                             SL_PULSE_TIMER, i);
        if (timer_create(CLOCK_MONOTONIC, &event, &s->timer[i]) == 0) {
            s->timer_created[i] = 1;
        }
    }

    s->ctp = dispatch_context_alloc(s->dpp);
    if (s->ctp == NULL) {
        ConnectDetach(s->coid);
        dispatch_destroy(s->dpp);
        free(s);
        return -1;
    }
    *out = s;
    return 0;
}

void sl_server_close(sl_server_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < SL_MAX_TIMERS; i++) {
        if (s->timer_created[i]) {
            timer_delete(s->timer[i]);
        }
    }
    if (s->coid != -1) {
        ConnectDetach(s->coid);
    }
    if (s->ctp) {
        dispatch_context_free(s->ctp);
    }
    if (s->dpp) {
        dispatch_destroy(s->dpp);
    }
    free(s);
    g_srv = NULL;
}

void sl_server_stop(sl_server_t *s)
{
    (void)s;
    /* dispatch_block() sits in the kernel; the daemon leaves its loop from
     * its signal handler (see src/sentineld/main.c). */
}

int sl_server_arm(sl_server_t *s, int slot, uint64_t abs_deadline_ns)
{
    struct itimerspec its;

    if (slot < 0 || slot >= SL_MAX_TIMERS || !s->timer_created[slot]) {
        return -1;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = (time_t)(abs_deadline_ns / 1000000000ull);
    its.it_value.tv_nsec = (long)(abs_deadline_ns % 1000000000ull);
    if (timer_settime(s->timer[slot], TIMER_ABSTIME, &its, NULL) != 0) {
        return -errno;
    }
    s->armed[slot] = 1;
    return 0;
}

int sl_server_disarm(sl_server_t *s, int slot)
{
    struct itimerspec its;

    if (slot < 0 || slot >= SL_MAX_TIMERS || !s->timer_created[slot]) {
        return -1;
    }
    memset(&its, 0, sizeof(its));
    if (timer_settime(s->timer[slot], TIMER_ABSTIME, &its, NULL) != 0) {
        return -errno;
    }
    s->armed[slot] = 0;
    return 0;
}

void sl_server_step(sl_server_t *s)
{
    s->ctp = dispatch_block(s->ctp);
    if (s->ctp == NULL) {
        return;
    }
    (void)dispatch_handler(s->ctp);
}

int sl_server_run(sl_server_t *s)
{
    if (s->ops.on_ready) {
        s->ops.on_ready(s->ctx);
    }
    for (;;) {
        sl_server_step(s);
    }
    return 0;
}

/* ---------------- client ---------------- */
int sl_client_open(sl_client_t **out, const char *path)
{
    sl_client_t *c;
    const char *p = path ? path : sl_default_path();

    c = (sl_client_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return -1;
    }
    c->fd = open(p, O_RDWR);
    if (c->fd < 0) {
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

int sl_client_call(sl_client_t *c, uint32_t dcmd,
                   const void *in, size_t inlen,
                   void *out, size_t outcap, size_t *outlen)
{
    size_t  bufsz = inlen > outcap ? inlen : outcap;
    void   *buf;
    int     n = 0;
    int     rc;

    if (bufsz == 0u) {
        bufsz = 1u;
    }
    buf = malloc(bufsz);
    if (buf == NULL) {
        return -ENOMEM;
    }
    if (inlen > 0u && in != NULL) {
        memcpy(buf, in, inlen);
    }

    /* devctl() takes one buffer whose size covers the direction encoded in
     * the command; the reply is written back into it. */
    rc = devctl(c->fd, (int)dcmd, buf, inlen ? inlen : outcap, &n);
    if (rc != EOK) {
        free(buf);
        return -rc;
    }
    if (out != NULL && outcap > 0u && n > 0) {
        size_t copy = ((size_t)n < outcap) ? (size_t)n : outcap;
        memcpy(out, buf, copy);
    }
    if (outlen) {
        *outlen = (size_t)n;
    }
    free(buf);
    return 0;
}

int sl_client_read(sl_client_t *c, char *buf, size_t cap, size_t *len)
{
    ssize_t n;

    do {
        n = read(c->fd, buf, cap - 1u);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        return -errno;
    }
    buf[n] = '\0';
    if (len) {
        *len = (size_t)n;
    }
    return 0;
}

void sl_client_close(sl_client_t *c)
{
    if (c == NULL) {
        return;
    }
    close(c->fd);
    free(c);
}
