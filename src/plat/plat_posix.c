/*
 * plat_posix.c - POSIX/Linux backend: AF_UNIX SOCK_SEQPACKET instead of a
 * QNX channel, timerfd instead of a pulse-delivered POSIX timer.
 *
 * The mapping is deliberately 1:1 with the QNX backend so the same
 * application loops and the same protocol run on both:
 *
 *   QNX                                  POSIX host
 *   ----------------------------------   ------------------------------
 *   ChannelCreate + resmgr_attach        socket/bind/listen (SEQPACKET)
 *   open("/dev/sentinel") + devctl()     connect() + dcmd datagram
 *   read(fd)                             dcmd SL_DCMD_READ
 *   timer_create(SIGEV_PULSE)            timerfd (absolute)
 *   sched priority (1..63)               SCHED_FIFO + mlockall
 *
 * Message boundaries are preserved by SOCK_SEQPACKET, which is what the
 * RTOS gives us for free with native message passing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_plat.h"
#include "sentinel.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SL_MAX_CLIENTS 64
#define SL_MAX_MSG     65536

typedef struct {
    uint32_t dcmd;
    uint32_t inlen;
} sl_req_hdr_t;

typedef struct {
    int32_t  status;
    uint32_t outlen;
} sl_rep_hdr_t;

struct sl_client {
    int  fd;
    char path[256];
};

struct sl_server {
    int             listen_fd;
    char            path[256];
    sl_server_ops_t ops;
    void           *ctx;
    int             client[SL_MAX_CLIENTS];
    int             nclients;
    int             tfd[SL_MAX_TIMERS];
    uint64_t        deadline[SL_MAX_TIMERS];
    int             stop;
};

const char *sl_default_path(void)
{
    const char *env = getenv("SENTINEL_PATH");
    return env ? env : "/tmp/sentinel.sock";
}

int sl_raise_priority(int prio)
{
    struct sched_param sp;
    int lprio;

    /* QNX priorities are 1..63; Linux SCHED_FIFO is 1..99. Scale so the
     * same configuration file is meaningful on both. */
    lprio = (prio * 99 + 62) / 63;
    if (lprio < 1) {
        lprio = 1;
    }
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = lprio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        return -errno;
    }
    (void)mlockall(MCL_CURRENT | MCL_FUTURE);
    return lprio;
}

/* ------------------------------------------------------------------ */
static void close_client(sl_server_t *s, int idx)
{
    close(s->client[idx]);
    s->client[idx] = s->client[s->nclients - 1];
    s->nclients--;
}

int sl_server_open(sl_server_t **out, const char *path,
                   const sl_server_ops_t *ops, void *ctx)
{
    sl_server_t *s;
    struct sockaddr_un addr;
    int i;

    s = (sl_server_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return -1;
    }
    s->ops = *ops;
    s->ctx = ctx;
    s->listen_fd = -1;
    for (i = 0; i < SL_MAX_TIMERS; i++) {
        s->tfd[i] = -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", path ? path : sl_default_path());

    unlink(s->path);
    s->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s->listen_fd < 0) {
        free(s);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->path);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s->listen_fd);
        free(s);
        return -1;
    }
    if (listen(s->listen_fd, 16) != 0) {
        close(s->listen_fd);
        unlink(s->path);
        free(s);
        return -1;
    }
    fcntl(s->listen_fd, F_SETFD, FD_CLOEXEC);
    *out = s;
    return 0;
}

void sl_server_close(sl_server_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->nclients; i++) {
        close(s->client[i]);
    }
    for (i = 0; i < SL_MAX_TIMERS; i++) {
        if (s->tfd[i] >= 0) {
            close(s->tfd[i]);
        }
    }
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
    }
    unlink(s->path);
    free(s);
}

void sl_server_stop(sl_server_t *s)
{
    if (s) {
        s->stop = 1;
    }
}

int sl_server_arm(sl_server_t *s, int slot, uint64_t abs_deadline_ns)
{
    struct itimerspec its;

    if (slot < 0 || slot >= SL_MAX_TIMERS) {
        return -1;
    }
    if (s->tfd[slot] < 0) {
        s->tfd[slot] = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (s->tfd[slot] < 0) {
            return -1;
        }
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = (time_t)(abs_deadline_ns / 1000000000ull);
    its.it_value.tv_nsec = (long)(abs_deadline_ns % 1000000000ull);
    if (timerfd_settime(s->tfd[slot], TFD_TIMER_ABSTIME, &its, NULL) != 0) {
        return -1;
    }
    s->deadline[slot] = abs_deadline_ns;
    return 0;
}

int sl_server_disarm(sl_server_t *s, int slot)
{
    struct itimerspec its;

    if (slot < 0 || slot >= SL_MAX_TIMERS || s->tfd[slot] < 0) {
        return 0;
    }
    memset(&its, 0, sizeof(its));
    if (timerfd_settime(s->tfd[slot], TFD_TIMER_ABSTIME, &its, NULL) != 0) {
        return -1;
    }
    s->deadline[slot] = 0;
    return 0;
}

static int next_timeout_ms(sl_server_t *s)
{
    uint64_t now = sl_now_ns();
    uint64_t best = 0;
    int i;

    for (i = 0; i < SL_MAX_TIMERS; i++) {
        if (s->deadline[i] != 0 && (best == 0 || s->deadline[i] < best)) {
            best = s->deadline[i];
        }
    }
    if (best == 0) {
        return 1000;
    }
    if (best <= now) {
        return 0;
    }
    return (int)((best - now) / 1000000ull) + 1;
}

static void handle_datagram(sl_server_t *s, int idx)
{
    static uint8_t buf[SL_MAX_MSG];
    static uint8_t out[SL_MAX_MSG];
    sl_req_hdr_t *req = (sl_req_hdr_t *)buf;
    sl_rep_hdr_t *rep = (sl_rep_hdr_t *)out;
    ssize_t n;

    n = recv(s->client[idx], buf, sizeof(buf), 0);
    if (n <= 0) {
        close_client(s, idx);
        return;
    }
    if ((size_t)n < sizeof(*req)) {
        return;
    }
    rep->status = 0;
    rep->outlen = 0;

    if (req->dcmd == SL_DCMD_READ && s->ops.on_read) {
        size_t len = 0;
        s->ops.on_read(s->ctx, (char *)out + sizeof(*rep),
                       sizeof(out) - sizeof(*rep), &len);
        rep->outlen = (uint32_t)len;
    } else if (s->ops.on_request) {
        size_t outlen = 0;
        const void *in = buf + sizeof(*req);
        if (req->inlen > (uint32_t)(n - (ssize_t)sizeof(*req))) {
            rep->status = -EINVAL;
        } else {
            rep->status = s->ops.on_request(s->ctx, req->dcmd, in, req->inlen,
                                            out + sizeof(*rep),
                                            sizeof(out) - sizeof(*rep), &outlen);
            rep->outlen = (uint32_t)outlen;
        }
    } else {
        rep->status = -ENOSYS;
    }
    if (send(s->client[idx], out, sizeof(*rep) + rep->outlen, 0) < 0) {
        close_client(s, idx);
    }
}

void sl_server_step(sl_server_t *s)
{
    struct pollfd pfd[SL_MAX_CLIENTS + SL_MAX_TIMERS + 1];
    int map_c[SL_MAX_CLIENTS + 1];
    int map_t[SL_MAX_TIMERS + 1];
    int nfd = 0, i, rc;

    pfd[nfd].fd = s->listen_fd;
    pfd[nfd].events = POLLIN;
    map_c[nfd] = -1;
    nfd++;

    for (i = 0; i < s->nclients; i++) {
        pfd[nfd].fd = s->client[i];
        pfd[nfd].events = POLLIN;
        map_c[nfd] = i;
        nfd++;
    }
    for (i = 0; i < SL_MAX_TIMERS; i++) {
        if (s->tfd[i] >= 0 && s->deadline[i] != 0) {
            pfd[nfd].fd = s->tfd[i];
            pfd[nfd].events = POLLIN;
            map_t[nfd] = i;
            nfd++;
        }
    }
    rc = poll(pfd, (nfds_t)nfd, next_timeout_ms(s));
    if (rc < 0) {
        if (errno == EINTR) {
            return;
        }
        s->stop = 1;
        return;
    }
    if (rc == 0) {
        /* Timeout: any deadline that has passed is a miss. */
        uint64_t now = sl_now_ns();
        for (i = 0; i < SL_MAX_TIMERS; i++) {
            if (s->deadline[i] != 0 && now >= s->deadline[i] && s->ops.on_timer) {
                s->deadline[i] = 0;
                s->ops.on_timer(s->ctx, i);
            }
        }
        return;
    }

    for (i = 0; i < nfd; i++) {
        if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }
        if (pfd[i].fd == s->listen_fd) {
            int c = accept(s->listen_fd, NULL, NULL);
            if (c >= 0 && s->nclients < SL_MAX_CLIENTS) {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                s->client[s->nclients++] = c;
            } else if (c >= 0) {
                close(c);
            }
        } else if (map_c[i] >= 0 && map_c[i] < s->nclients &&
                   pfd[i].fd == s->client[map_c[i]]) {
            handle_datagram(s, map_c[i]);
        } else {
            for (int t = 0; t < SL_MAX_TIMERS; t++) {
                if (s->tfd[t] == pfd[i].fd) {
                    uint64_t exp = 0;
                    ssize_t r = read(s->tfd[t], &exp, sizeof(exp));
                    (void)r;
                    s->deadline[t] = 0;
                    if (s->ops.on_timer) {
                        s->ops.on_timer(s->ctx, t);
                    }
                    break;
                }
            }
        }
    }
}

int sl_server_run(sl_server_t *s)
{
    if (s->ops.on_ready) {
        s->ops.on_ready(s->ctx);
    }
    while (!s->stop) {
        sl_server_step(s);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
int sl_client_open(sl_client_t **out, const char *path)
{
    sl_client_t *c;
    struct sockaddr_un addr;

    c = (sl_client_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return -1;
    }
    c->fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (c->fd < 0) {
        free(c);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(c->path, sizeof(c->path), "%s", path ? path : sl_default_path());
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", c->path);
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(c->fd);
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

int sl_client_call(sl_client_t *c, uint32_t dcmd,
                   const void *in, size_t inlen,
                   void *out_, size_t outcap, size_t *outlen)
{
    static uint8_t buf[SL_MAX_MSG];
    static uint8_t rbuf[SL_MAX_MSG];
    sl_req_hdr_t *req = (sl_req_hdr_t *)buf;
    sl_rep_hdr_t *rep = (sl_rep_hdr_t *)rbuf;
    ssize_t n;

    if (inlen > SL_MAX_MSG - sizeof(*req)) {
        return -EINVAL;
    }
    req->dcmd = dcmd;
    req->inlen = (uint32_t)inlen;
    if (inlen) {
        memcpy(buf + sizeof(*req), in, inlen);
    }
    if (send(c->fd, buf, sizeof(*req) + inlen, 0) < 0) {
        return -errno;
    }
    do {
        n = recv(c->fd, rbuf, sizeof(rbuf), 0);
    } while (n < 0 && errno == EINTR);
    if (n < (ssize_t)sizeof(*rep)) {
        return -EIO;
    }
    if (rep->status < 0) {
        return (int)rep->status;
    }
    if (outlen) {
        *outlen = rep->outlen;
    }
    if (out_ && rep->outlen) {
        size_t copy = rep->outlen < outcap ? rep->outlen : outcap;
        memcpy(out_, rbuf + sizeof(*rep), copy);
    }
    return 0;
}

int sl_client_read(sl_client_t *c, char *buf, size_t cap, size_t *len)
{
    size_t got = 0;
    int rc = sl_client_call(c, SL_DCMD_READ, NULL, 0, buf, cap - 1, &got);
    if (rc < 0) {
        return rc;
    }
    if (got >= cap) {
        got = cap - 1;
    }
    buf[got] = '\0';
    if (len) {
        *len = got;
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
