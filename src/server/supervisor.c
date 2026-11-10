/*
 * supervisor.c - the parent of everything.
 *
 *   - initialises the database once (first admin) BEFORE starting children,
 *   - starts N event-loop processes,
 *   - accepts new TCP clients and passes each to the least-busy loop
 *     (the socket is sent over a unix socket, SCM_RIGHTS),
 *   - restarts a loop if it dies (with a delay if it keeps dying),
 *   - on SIGTERM / Ctrl-C asks the loops to finish and exits.
 *
 * If the supervisor itself dies, every loop sees its control socket close,
 * finishes its running requests and exits.  Nothing is corrupted: the
 * journal protects data, the kernel releases all locks.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "server.h"

#define WHO "supervisor"

typedef struct {
    pid_t   pid;
    int     ctrl;          /* our end of the socketpair to the loop     */
    long    clients;       /* clients we passed minus clients it closed  */
    int64_t respawn_at;
    int64_t started_ms;
    int     fast_deaths;
} LoopInfo;

static const ServerConfig *cfg;
static LoopInfo *loops;
static int ep;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void start_loop(int i)
{
    int sv[2];
    LoopInfo *l = &loops[i];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) { l->respawn_at = now_ms() + 1000; return; }
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); l->respawn_at = now_ms() + 1000; return; }
    if (pid == 0) {                       /* child = event loop */
        close(sv[0]);
        loop_main(cfg, sv[1]);            /* never returns */
        _exit(1);
    }
    close(sv[1]);
    net_set_nonblock(sv[0]);
    l->pid = pid;
    l->ctrl = sv[0];
    l->clients = 0;
    l->started_ms = now_ms();
    struct epoll_event e = { .events = EPOLLIN, .data.u64 = ((uint64_t)2 << 32) | (uint32_t)i };
    epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &e);
    log_msg(WHO, "loop %d started (pid %d)", i, (int)pid);
}

/* A loop is gone: forget it, restart later (slower if it keeps crashing). */
static void loop_gone(int i, const char *why)
{
    LoopInfo *l = &loops[i];
    if (l->pid <= 0) return;
    log_msg(WHO, "loop %d (pid %d) is gone: %s", i, (int)l->pid, why);
    kill(l->pid, SIGKILL);                /* its workers die with it (PDEATHSIG / EOF) */
    close(l->ctrl);
    l->ctrl = -1;
    l->pid = -1;
    l->clients = 0;
    int64_t now = now_ms();
    if (now - l->started_ms < 1000) l->fast_deaths++; else l->fast_deaths = 0;
    int64_t delay = l->fast_deaths > 1 ? (100LL << (l->fast_deaths > 5 ? 5 : l->fast_deaths)) : 0;
    l->respawn_at = now + delay;
}

/* The loop with the fewest clients (only loops that are alive). */
static int least_loaded(void)
{
    int best = -1;
    for (int i = 0; i < cfg->nloops; i++)
        if (loops[i].pid > 0 && (best < 0 || loops[i].clients < loops[best].clients)) best = i;
    return best;
}

static void accept_clients(int lfd, int *spare)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                /* Out of descriptors: free the spare one, accept + drop the
                 * waiting client (otherwise epoll would spin on it), re-take the spare. */
                close(*spare);
                int d = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
                if (d >= 0) close(d);
                *spare = open("/dev/null", O_RDONLY | O_CLOEXEC);
                continue;
            }
            return;                       /* EAGAIN: nothing more to accept */
        }
        int i = least_loaded();
        if (i < 0 || net_send_fd(loops[i].ctrl, fd) < 0) { close(fd); continue; }   /* no loop: drop */
        loops[i].clients++;
        close(fd);                        /* the loop has its own copy now */
    }
}

/* Read the "client closed" bytes a loop sends us (one '-' per closed client). */
static void on_loop_message(int i)
{
    char buf[256];
    LoopInfo *l = &loops[i];
    for (;;) {
        ssize_t n = recv(l->ctrl, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) { l->clients -= n; if (l->clients < 0) l->clients = 0; continue; }
        if (n == 0) { loop_gone(i, "control socket closed"); return; }
        return;                           /* EAGAIN */
    }
}

static void init_database(void)
{
    Db db;
    mkdir(cfg->datadir, 0755);
    if (getenv("BANK_NO_FSYNC")) db_sync_enabled = 0;
    if (db_open(&db, cfg->datadir) < 0) { log_msg(WHO, "cannot open database in %s", cfg->datadir); exit(1); }
    if (bank_create_first_admin(&db, "admin", "admin123") != BANK_OK) { log_msg(WHO, "cannot create admin"); exit(1); }
    db_close(&db);                        /* IMPORTANT: close before fork, children must not share our slot */
}

void supervisor_main(const ServerConfig *config)
{
    cfg = config;
    log_verbose = cfg->verbose;

    if (cfg->nloops * cfg->nworkers + 2 > JOURNAL_SLOTS) {
        log_msg(WHO, "too many workers: loops*workers must be <= %d", JOURNAL_SLOTS - 2);
        exit(1);
    }
    long limit = net_raise_fd_limit();
    init_database();

    int lfd = net_listen(cfg->port);
    if (lfd < 0) { log_msg(WHO, "cannot listen on port %d: %s", cfg->port, strerror(errno)); exit(1); }
    int spare = open("/dev/null", O_RDONLY | O_CLOEXEC);

    prctl(PR_SET_CHILD_SUBREAPER, 1);     /* orphans (if a loop dies) come to us, not init */
    signal(SIGPIPE, SIG_IGN);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD); sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);

    ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event e = { .events = EPOLLIN, .data.u64 = ((uint64_t)1 << 32) };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &e);
    e.data.u64 = ((uint64_t)3 << 32);
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &e);

    loops = calloc(cfg->nloops, sizeof(LoopInfo));
    for (int i = 0; i < cfg->nloops; i++) { loops[i].pid = -1; loops[i].ctrl = -1; }
    for (int i = 0; i < cfg->nloops; i++) start_loop(i);
    log_msg(WHO, "listening on port %d, %d loops x %d workers, fd limit %ld, data dir %s",
            cfg->port, cfg->nloops, cfg->nworkers, limit, cfg->datadir);

    int stopping = 0;
    int64_t stop_deadline = 0;
    struct epoll_event evs[64];

    for (;;) {
        int n = epoll_wait(ep, evs, 64, 200);          /* 200 ms: also our restart timer */
        for (int k = 0; k < n; k++) {
            int tag = (int)(evs[k].data.u64 >> 32);
            int val = (int)(evs[k].data.u64 & 0xffffffffu);
            if (tag == 1 && !stopping) accept_clients(lfd, &spare);
            else if (tag == 2) on_loop_message(val);
            else if (tag == 3) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                    if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        if (!stopping) {
                            log_msg(WHO, "stopping");
                            stopping = 1;
                            stop_deadline = now_ms() + (int64_t)(cfg->worker_timeout_secs + 6) * 1000;
                            epoll_ctl(ep, EPOLL_CTL_DEL, lfd, NULL);
                            close(lfd);                   /* no new clients */
                            for (int i = 0; i < cfg->nloops; i++)
                                if (loops[i].pid > 0) kill(loops[i].pid, SIGTERM);
                        }
                    }
                }
                int st;
                pid_t pid;
                while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
                    for (int i = 0; i < cfg->nloops; i++)
                        if (loops[i].pid == pid) loop_gone(i, WIFSIGNALED(st) ? "killed by a signal" : "exited");
            }
        }

        int64_t now = now_ms();
        if (!stopping) {
            for (int i = 0; i < cfg->nloops; i++)
                if (loops[i].pid < 0 && now >= loops[i].respawn_at) start_loop(i);
        } else {
            int alive = 0;
            for (int i = 0; i < cfg->nloops; i++) if (loops[i].pid > 0) alive++;
            if (alive == 0 || now > stop_deadline) {
                for (int i = 0; i < cfg->nloops; i++) if (loops[i].pid > 0) kill(loops[i].pid, SIGKILL);
                log_msg(WHO, "bye");
                exit(0);
            }
        }
    }
}
