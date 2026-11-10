/*
 * loop.c - ONE event-loop process (epoll).
 *
 * What this process does:
 *   - receives client sockets from the supervisor (over a unix socket),
 *   - reads 512-byte requests from clients without ever blocking,
 *   - hands each request to an idle worker process (or queues it),
 *   - receives the finished reply from the worker and sends it to the client,
 *   - starts its worker processes, notices when one dies, replaces it,
 *   - disconnects clients that are silent for too long.
 *
 * It never touches the database files itself (workers do), so a slow disk
 * can never freeze the event loop.
 *
 * Data layout (all O(1)):
 *   clients[fd]  one slot per possible file descriptor number.  The kernel
 *                hands out small fd numbers, so the fd IS the index: no
 *                searching, no hash table.
 *   workers[]    a handful of entries (default 8): scanning them is nothing.
 *   queue        a ring buffer of waiting requests (client fd + generation).
 *   LRU list     clients ordered by last activity -> idle timeout only looks
 *                at the oldest end, it never scans everybody.
 *
 * "generation" number: when a client closes, its fd number is reused by the
 * next client.  A worker reply that arrives for the OLD client must not be
 * delivered to the NEW one.  So every client slot has a generation counter,
 * increased at each close, and replies carry the generation they were
 * started with.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "net.h"
#include "server.h"

#define WHO "loop"

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int      active;           /* slot in use?                                  */
    uint32_t gen;              /* generation (see top of file)                  */
    int      busy;             /* a request of this client is at a worker/queue */
    uint32_t events;           /* epoll events currently registered             */
    int      in_len;           /* bytes of the request received so far          */
    int      out_len;          /* >0: a reply is waiting to be sent             */
    int      out_sent;
    int      user_id;          /* logged-in user, 0 = not logged in             */
    int      session_fd;       /* the session lock file (-1 = none)             */
    int      lru_prev, lru_next;
    int64_t  last_ms;          /* last time this client did something           */
    Packet   in_pkt;           /* request being received / being processed      */
    Packet   out_pkt;          /* reply being sent                              */
} Client;

enum { W_DEAD = 0, W_IDLE, W_BUSY };

typedef struct {
    pid_t    pid;
    int      chan;             /* our end of the socketpair to the worker       */
    int      state;
    int      cfd;              /* client being served (when BUSY)               */
    uint32_t cgen;
    int64_t  t_start;          /* when the current request was handed over      */
    int64_t  spawned_ms;
    int64_t  respawn_at;       /* when a DEAD worker may be restarted           */
    int      fast_deaths;      /* deaths soon after start (crash loop counter)  */
} Worker;

typedef struct { int fd; uint32_t gen; } Job;

/* epoll event tags: what does an event belong to? */
enum { TAG_CLIENT = 1, TAG_CTRL, TAG_SIGNAL, TAG_TIMER, TAG_WORKER };
#define EV(tag, val)  (((uint64_t)(tag) << 32) | (uint32_t)(val))

static const ServerConfig *cfg;
static int  ep, ctrl, sigfd, tfd;

static Client *clients;        /* clients[fd]                  */
static long    cap;            /* number of slots              */
static long    nactive;        /* clients currently connected  */
static int     lru_head = -1;  /* oldest activity              */
static int     lru_tail = -1;  /* newest activity              */

static Worker *workers;
static int     nworkers;

static Job    *queue;          /* ring buffer of waiting requests */
static long    q_head, q_len;

static long    owed;           /* "client closed" notes not yet sent to the supervisor */
static int     shutting_down;
static int64_t shutdown_deadline;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ep_ctl(int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = events;
    e.data.u64 = data;
    epoll_ctl(ep, op, fd, &e);
}

/* Operations that cannot change anything: safe to retry after a worker crash. */
static int op_is_readonly(int op)
{
    switch (op) {
    case OP_LOGIN: case OP_BALANCE: case OP_HISTORY: case OP_MY_LOANS:
    case OP_ASSIGNED_LOANS: case OP_PENDING_LOANS: case OP_FEEDBACK_LIST:
    case OP_GET_USER: case OP_FIND_USER:
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* LRU list: clients ordered by last activity (oldest at the head)     */
/* ------------------------------------------------------------------ */

static void lru_remove(int fd)
{
    Client *c = &clients[fd];
    if (c->lru_prev >= 0) clients[c->lru_prev].lru_next = c->lru_next; else lru_head = c->lru_next;
    if (c->lru_next >= 0) clients[c->lru_next].lru_prev = c->lru_prev; else lru_tail = c->lru_prev;
    c->lru_prev = c->lru_next = -1;
}

static void lru_push_tail(int fd)
{
    Client *c = &clients[fd];
    c->lru_prev = lru_tail;
    c->lru_next = -1;
    if (lru_tail >= 0) clients[lru_tail].lru_next = fd; else lru_head = fd;
    lru_tail = fd;
}

static void touch(int fd)
{
    clients[fd].last_ms = now_ms();
    lru_remove(fd);
    lru_push_tail(fd);
}

/* ------------------------------------------------------------------ */
/* Telling the supervisor that a client is gone (it counts our load)   */
/* ------------------------------------------------------------------ */

static void flush_owed(void)
{
    while (owed > 0 && net_send_byte(ctrl, '-') == 0) owed--;
}

/* ------------------------------------------------------------------ */
/* Clients: add, close, epoll interest                                 */
/* ------------------------------------------------------------------ */

/* Decide what we want to hear about this client right now:
 *   reply waiting to be sent -> tell me when the socket can take data (OUT)
 *   idle (no request running) -> tell me when a request arrives (IN)
 *   request running           -> nothing: we read its next request only
 *                                after its reply was sent. */
static void client_update_events(int fd)
{
    Client *c = &clients[fd];
    uint32_t want = 0;

    if (c->out_len > 0)                       want = EPOLLOUT;
    else if (!c->busy && !shutting_down)      want = EPOLLIN;
    if (want != c->events) {
        ep_ctl(EPOLL_CTL_MOD, fd, want, EV(TAG_CLIENT, fd));
        c->events = want;
    }
}

static void close_client(int fd)
{
    Client *c = &clients[fd];
    if (!c->active) return;

    lru_remove(fd);
    if (c->session_fd >= 0) close(c->session_fd);   /* logs the user out */
    close(fd);                                      /* also removes it from epoll */
    c->active = 0;
    c->busy = 0;
    c->gen++;                                       /* old replies must not match the next client */
    c->session_fd = -1;
    c->user_id = 0;
    nactive--;
    owed++;
    flush_owed();
}

static void add_client(int fd)
{
    Client *c;

    if (shutting_down || fd >= cap || (cfg->max_clients_per_loop > 0 && nactive >= cfg->max_clients_per_loop)) {
        Packet busy;                                /* tell the client why, then drop it */
        memset(&busy, 0, sizeof busy);
        busy.magic = PROTO_MAGIC;
        busy.status = PROTO_E_SERVER_BUSY;
        send(fd, &busy, sizeof busy, MSG_NOSIGNAL | MSG_DONTWAIT);
        close(fd);
        owed++;
        flush_owed();
        return;
    }

    net_tune_client(fd);                            /* non-blocking, keepalive, NODELAY */
    c = &clients[fd];
    uint32_t gen = c->gen;                          /* keep the generation counter */
    memset(c, 0, sizeof *c);
    c->gen = gen;
    c->active = 1;
    c->session_fd = -1;
    c->lru_prev = c->lru_next = -1;
    c->events = EPOLLIN;
    ep_ctl(EPOLL_CTL_ADD, fd, EPOLLIN, EV(TAG_CLIENT, fd));
    nactive++;
    c->last_ms = now_ms();
    lru_push_tail(fd);                              /* (touch() would first "remove" it from a list it is not in) */
    log_debug(WHO, "client fd %d connected (%ld active)", fd, nactive);
}

/* ------------------------------------------------------------------ */
/* Sending replies                                                     */
/* ------------------------------------------------------------------ */

/* Push as much of the waiting reply into the socket as it accepts. */
static void flush_output(int fd)
{
    Client *c = &clients[fd];

    while (c->out_len > 0) {
        ssize_t n = send(fd, (char *)&c->out_pkt + c->out_sent, c->out_len - c->out_sent,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { client_update_events(fd); return; }
            close_client(fd);
            return;
        }
        c->out_sent += (int)n;
        touch(fd);
        if (c->out_sent == c->out_len) c->out_len = 0;
    }
    if (shutting_down) { close_client(fd); return; }
    client_update_events(fd);                       /* reply done: listen for the next request */
}

/* Queue a finished reply (already in c->out_pkt) and try to send it now. */
static void start_output(int fd)
{
    Client *c = &clients[fd];
    c->in_len = 0;                                  /* the request is finished */
    c->out_len = PACKET_SIZE;
    c->out_sent = 0;
    flush_output(fd);
}

/* Reply with just a status (no worker involved). */
static void reply_status(int fd, int status)
{
    Client *c = &clients[fd];
    memset(&c->out_pkt, 0, sizeof c->out_pkt);
    c->out_pkt.magic  = PROTO_MAGIC;
    c->out_pkt.op     = c->in_pkt.op;
    c->out_pkt.seq    = c->in_pkt.seq;
    c->out_pkt.status = status;
    start_output(fd);
}

/* ------------------------------------------------------------------ */
/* Workers                                                             */
/* ------------------------------------------------------------------ */

static void spawn_worker(int i)
{
    Worker *w = &workers[i];
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        w->respawn_at = now_ms() + 1000;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        w->respawn_at = now_ms() + 1000;
        return;
    }
    if (pid == 0) {                                 /* ---- child: become a worker ---- */
        close(sv[0]);
        worker_main(cfg, sv[1]);                    /* never returns */
        _exit(1);
    }
    close(sv[1]);                                   /* ---- parent ---- */
    net_set_nonblock(sv[0]);
    w->pid = pid;
    w->chan = sv[0];
    w->state = W_IDLE;
    w->cfd = -1;
    w->spawned_ms = now_ms();
    ep_ctl(EPOLL_CTL_ADD, sv[0], EPOLLIN, EV(TAG_WORKER, i));
    log_debug(WHO, "worker %d started (pid %d)", i, (int)pid);
}

static int find_idle_worker(void)
{
    for (int i = 0; i < nworkers; i++)
        if (workers[i].state == W_IDLE) return i;
    return -1;
}

/* Give the request of client `fd` to worker `wi`.  0 = sent, -1 = failed. */
static int send_job(int wi, int fd)
{
    Worker *w = &workers[wi];
    Client *c = &clients[fd];
    WorkerReq rq;

    rq.client_fd  = fd;
    rq.client_gen = c->gen;
    rq.user_id    = c->user_id;
    rq.pad        = 0;
    rq.pkt        = c->in_pkt;
    if (send(w->chan, &rq, sizeof rq, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof rq)
        return -1;

    w->state   = W_BUSY;
    w->cfd     = fd;
    w->cgen    = c->gen;
    w->t_start = now_ms();
    return 0;
}

static void queue_push(int fd, uint32_t gen)
{
    queue[(q_head + q_len) % cap].fd = fd;
    queue[(q_head + q_len) % cap].gen = gen;
    q_len++;
}

static void worker_died(int wi, const char *why);

/* While there is a waiting request and an idle worker: connect them. */
static void dispatch_queue(void)
{
    while (q_len > 0) {
        int wi = find_idle_worker();
        if (wi < 0) return;
        Job j = queue[q_head];
        q_head = (q_head + 1) % cap;
        q_len--;

        Client *c = &clients[j.fd];
        if (!c->active || c->gen != j.gen || !c->busy) continue;   /* client left meanwhile */
        if (send_job(wi, j.fd) < 0) {
            queue_push(j.fd, j.gen);                /* not delivered: try again later */
            worker_died(wi, "send failed");
        }
    }
}

/* A request that was running in a dead worker: tell the client. */
static void fail_job(int fd, uint32_t gen)
{
    Client *c = &clients[fd];
    if (!c->active || c->gen != gen || !c->busy) return;
    c->busy = 0;
    /* Read-only request: nothing can have happened, "try again" is accurate.
     * Writing request: we do NOT know whether it was committed. */
    reply_status(fd, op_is_readonly(c->in_pkt.op) ? PROTO_E_SERVER_BUSY : PROTO_E_UNKNOWN_RESULT);
}

/* A worker is gone (crashed, was killed, or stopped answering). */
static void worker_died(int wi, const char *why)
{
    Worker *w = &workers[wi];
    int64_t now = now_ms();

    if (w->state == W_DEAD) return;
    int was_busy = (w->state == W_BUSY);
    int cfd = w->cfd;
    uint32_t cgen = w->cgen;

    log_msg(WHO, "worker %d (pid %d) is gone: %s%s", wi, (int)w->pid, why, was_busy ? " [was busy]" : "");
    if (w->pid > 0) kill(w->pid, SIGKILL);          /* make sure; safe thanks to the journal */
    close(w->chan);                                 /* also removes it from epoll */
    w->chan = -1;
    w->pid = -1;
    w->state = W_DEAD;

    /* Crash loop protection: if workers keep dying right after start, wait
     * longer and longer before the next restart (up to 2 s). */
    if (now - w->spawned_ms < 1000) w->fast_deaths++; else w->fast_deaths = 0;
    int64_t delay = 0;
    if (w->fast_deaths > 1) {
        delay = 50LL << (w->fast_deaths > 6 ? 6 : w->fast_deaths - 1);
        if (delay > 2000) delay = 2000;
    }
    w->respawn_at = now + delay;

    if (was_busy) fail_job(cfd, cgen);
}

/* A reply from worker `wi` arrived: deliver it. */
static void on_worker_readable(int wi)
{
    Worker *w = &workers[wi];
    WorkerResp rs;

    if (w->state == W_DEAD) return;
    ssize_t n = recv(w->chan, &rs, sizeof rs, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n != (ssize_t)sizeof rs) { worker_died(wi, n == 0 ? "channel closed" : "bad message"); return; }

    if (w->state != W_BUSY) return;                 /* unexpected, ignore */
    w->state = W_IDLE;                              /* free for the next job */

    Client *c = (rs.client_fd >= 0 && rs.client_fd < cap) ? &clients[rs.client_fd] : NULL;
    if (c && c->active && c->gen == rs.client_gen && c->busy) {
        int fd = rs.client_fd;

        /* A successful LOGIN: now take the one-session-per-user lock.  This is
         * the single place that decides "already logged in elsewhere" - it is
         * a kernel lock, so it works across all loops and workers. */
        if (rs.login_user_id > 0 && rs.pkt.status == BANK_OK) {
            int sfd = session_acquire_dir(cfg->datadir, rs.login_user_id);
            if (sfd < 0) {
                memset(rs.pkt.payload, 0, sizeof rs.pkt.payload);   /* do not leak the login data */
                rs.pkt.status = sfd;                                 /* ALREADY_LOGGED_IN or IO */
            } else {
                c->session_fd = sfd;
                c->user_id = rs.login_user_id;
                log_debug(WHO, "user %d logged in (client fd %d)", c->user_id, fd);
            }
        }
        c->busy = 0;
        c->out_pkt = rs.pkt;
        start_output(fd);
    }
    dispatch_queue();
}

/* ------------------------------------------------------------------ */
/* Reading requests from clients                                       */
/* ------------------------------------------------------------------ */

/* A complete 512-byte request is in c->in_pkt.  Decide what to do with it. */
static void handle_request(int fd)
{
    Client *c = &clients[fd];
    Packet *p = &c->in_pkt;

    /* Anything that is not a valid packet header: the stream is broken, drop the client. */
    if (p->magic != PROTO_MAGIC || p->op < OP_LOGIN || p->op >= OP_LAST) {
        log_debug(WHO, "client fd %d sent a bad packet, closing", fd);
        close_client(fd);
        return;
    }

    /* These three need no worker. */
    if (p->op == OP_LOGOUT) {
        if (c->user_id == 0) { reply_status(fd, PROTO_E_NOT_LOGGED_IN); return; }
        if (c->session_fd >= 0) close(c->session_fd);
        c->session_fd = -1;
        c->user_id = 0;
        reply_status(fd, BANK_OK);
        return;
    }
    if (p->op == OP_LOGIN && c->user_id != 0) { reply_status(fd, BANK_E_ALREADY_LOGGED_IN); return; }
    if (p->op != OP_LOGIN && c->user_id == 0) { reply_status(fd, PROTO_E_NOT_LOGGED_IN); return; }

    /* Real work: give it to a worker (or wait in the queue). */
    c->busy = 1;
    client_update_events(fd);                       /* stop reading until the reply is sent */
    int wi = find_idle_worker();
    if (wi >= 0 && send_job(wi, fd) == 0) return;
    if (wi >= 0) worker_died(wi, "send failed");
    queue_push(fd, c->gen);
}

static void on_client_readable(int fd)
{
    Client *c = &clients[fd];

    while (c->active && !c->busy && c->out_len == 0 && c->in_len < PACKET_SIZE) {
        ssize_t n = recv(fd, (char *)&c->in_pkt + c->in_len, PACKET_SIZE - c->in_len, MSG_DONTWAIT);
        if (n > 0) {
            c->in_len += (int)n;
            touch(fd);
        } else if (n == 0) {
            close_client(fd);                       /* client hung up */
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;                                 /* nothing more right now */
        } else {
            close_client(fd);
            return;
        }
    }
    if (c->active && c->in_len == PACKET_SIZE && !c->busy && c->out_len == 0)
        handle_request(fd);
}

/* ------------------------------------------------------------------ */
/* Supervisor link, signals, timer                                     */
/* ------------------------------------------------------------------ */

static void begin_shutdown(const char *why)
{
    if (shutting_down) return;
    shutting_down = 1;
    shutdown_deadline = now_ms() + (int64_t)(cfg->worker_timeout_secs + 3) * 1000;
    log_msg(WHO, "shutting down (%s): %ld clients", why, nactive);

    /* Close every client that has nothing running; busy ones are closed
     * right after their reply was sent. */
    for (int fd = lru_head; fd >= 0; ) {
        int next = clients[fd].lru_next;
        if (!clients[fd].busy && clients[fd].out_len == 0) close_client(fd);
        else client_update_events(fd);
        fd = next;
    }
}

static void on_ctrl_readable(void)
{
    for (;;) {
        int fd = -1;
        char b;
        int r = net_recv_fd(ctrl, &fd, &b);
        if (r == 1) add_client(fd);
        else if (r == 2) continue;
        else if (r == 0) {                          /* supervisor is gone */
            ep_ctl(EPOLL_CTL_DEL, ctrl, 0, 0);
            begin_shutdown("supervisor died");
            return;
        } else return;                              /* EAGAIN: all read */
    }
}

static void on_signal(void)
{
    struct signalfd_siginfo si;

    while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) {
        if (si.ssi_signo == SIGTERM) begin_shutdown("SIGTERM");
        else if (si.ssi_signo == SIGCHLD) {
            int st;
            pid_t pid;
            while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
                for (int i = 0; i < nworkers; i++)
                    if (workers[i].pid == pid) {
                        worker_died(i, WIFSIGNALED(st) ? "killed by a signal" : "exited");
                        break;
                    }
            }
        }
    }
}

/* Runs 5 times a second: watchdog, restarts, idle clients. */
static void on_tick(void)
{
    uint64_t ticks;
    int64_t now = now_ms();

    if (read(tfd, &ticks, sizeof ticks) < 0) { /* nothing */ }

    /* 1. Watchdog: a worker stuck on one request for too long is killed.
     *    (Killing at any moment is safe: the journal makes every request
     *    all-or-nothing.) */
    for (int i = 0; i < nworkers; i++)
        if (workers[i].state == W_BUSY && now - workers[i].t_start > (int64_t)cfg->worker_timeout_secs * 1000)
            worker_died(i, "request took too long (watchdog)");

    /* 2. Restart dead workers. */
    if (!shutting_down)
        for (int i = 0; i < nworkers; i++)
            if (workers[i].state == W_DEAD && now >= workers[i].respawn_at) spawn_worker(i);
    dispatch_queue();

    /* 3. Disconnect silent clients (only the oldest end of the LRU list is looked at). */
    if (cfg->idle_secs > 0) {
        int64_t limit = (int64_t)cfg->idle_secs * 1000;
        while (lru_head >= 0 && now - clients[lru_head].last_ms > limit) {
            int fd = lru_head;
            if (clients[fd].busy) { touch(fd); continue; }   /* running a request: not idle */
            log_debug(WHO, "client fd %d idle too long, closing", fd);
            close_client(fd);
        }
    }

    flush_owed();

    if (shutting_down && (nactive == 0 || now > shutdown_deadline)) {
        log_msg(WHO, "exit");
        _exit(0);
    }
}

/* ------------------------------------------------------------------ */
/* Start-up and main loop                                              */
/* ------------------------------------------------------------------ */

/* Big arrays: reserve address space only, memory is used when touched.
 * MADV_DONTFORK: do not copy them into worker processes. */
static void *big_alloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    madvise(p, bytes, MADV_DONTFORK);
    return p;
}

void loop_main(const ServerConfig *config, int ctrl_fd)
{
    cfg = config;
    ctrl = ctrl_fd;

    /* Keep only: stdin/out/err and the link to the supervisor.
     * (We inherited the listening socket and other loops' links.) */
    net_close_all_except(&ctrl, 1);

    /* Signals: SIGTERM = "shut down", SIGCHLD = "a worker changed state".
     * Both are read from a file descriptor (signalfd) inside epoll. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
    signal(SIGINT, SIG_IGN);                        /* the supervisor handles Ctrl-C and tells us */
    signal(SIGPIPE, SIG_IGN);

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    cap = (long)rl.rlim_cur;
    if (cap > (1L << 20)) cap = 1L << 20;           /* 1M clients per loop is plenty */

    clients = big_alloc(sizeof(Client) * cap);
    queue   = big_alloc(sizeof(Job) * cap);
    workers = calloc(cfg->nworkers, sizeof(Worker));
    if (!clients || !queue || !workers) { log_msg(WHO, "out of memory"); _exit(2); }
    nworkers = cfg->nworkers;
    for (int i = 0; i < nworkers; i++) { workers[i].pid = -1; workers[i].chan = -1; }

    ep    = epoll_create1(EPOLL_CLOEXEC);
    sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    tfd   = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ep < 0 || sigfd < 0 || tfd < 0) { log_msg(WHO, "epoll setup failed"); _exit(2); }

    struct itimerspec its = { .it_interval = { 0, 200 * 1000 * 1000 }, .it_value = { 0, 200 * 1000 * 1000 } };
    timerfd_settime(tfd, 0, &its, NULL);

    net_set_nonblock(ctrl);
    ep_ctl(EPOLL_CTL_ADD, ctrl,  EPOLLIN, EV(TAG_CTRL, 0));
    ep_ctl(EPOLL_CTL_ADD, sigfd, EPOLLIN, EV(TAG_SIGNAL, 0));
    ep_ctl(EPOLL_CTL_ADD, tfd,   EPOLLIN, EV(TAG_TIMER, 0));

    for (int i = 0; i < nworkers; i++) spawn_worker(i);
    log_msg(WHO, "started: %d workers, up to %ld clients", nworkers, cap);

    struct epoll_event evs[256];
    for (;;) {
        int n = epoll_wait(ep, evs, 256, -1);
        if (n < 0) { if (errno == EINTR) continue; log_msg(WHO, "epoll_wait failed"); _exit(2); }

        for (int k = 0; k < n; k++) {
            uint32_t tag = (uint32_t)(evs[k].data.u64 >> 32);
            int      val = (int)(evs[k].data.u64 & 0xffffffffu);
            uint32_t e   = evs[k].events;

            switch (tag) {
            case TAG_CLIENT:
                if (!clients[val].active) break;                 /* closed earlier in this batch */
                if (e & (EPOLLERR | EPOLLHUP)) { close_client(val); break; }
                if (e & EPOLLOUT) flush_output(val);
                if ((e & EPOLLIN) && clients[val].active) on_client_readable(val);
                break;
            case TAG_WORKER:  on_worker_readable(val); break;
            case TAG_CTRL:    on_ctrl_readable();      break;
            case TAG_SIGNAL:  on_signal();             break;
            case TAG_TIMER:   on_tick();               break;
            }
        }
        if (shutting_down && nactive == 0) { log_msg(WHO, "exit"); _exit(0); }
    }
}
