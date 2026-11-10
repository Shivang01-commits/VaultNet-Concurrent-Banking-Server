/*
 * server_helpers.h - helpers for tests that start a REAL bank_server process
 * and talk to it over TCP (used by test_server_basic.c / test_server_crash.c).
 *
 *   srv_start / srv_stop       run bin/bank_server in a temporary data directory
 *   proc_children / proc_state look at the process tree through /proc
 *   admin / staff helpers      create users quickly through the protocol
 */
#ifndef SERVER_HELPERS_H
#define SERVER_HELPERS_H

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/resource.h>
#include <time.h>

#include "test_helpers.h"
#include "client.h"
#include "net.h"

typedef struct {
    pid_t pid;                 /* supervisor process id */
    int   port;
    char  dir[64];             /* data directory        */
} Srv;

static inline long now_ms_t(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* One attempt on one port.  0 = server is up, -2 = the server process exited
 * (port busy?), -1 = it did not answer in time. */
static inline int srv_try(Srv *s, int loops, int workers, const char *const *extra, int crash_every)
{
    char *argv[32];
    int n = 0;
    char sloops[16], sworkers[16], sport[16];

    snprintf(sloops, sizeof sloops, "%d", loops);
    snprintf(sworkers, sizeof sworkers, "%d", workers);
    snprintf(sport, sizeof sport, "%d", s->port);

    argv[n++] = "bin/bank_server";
    argv[n++] = "--port";    argv[n++] = sport;
    argv[n++] = "--data";    argv[n++] = s->dir;
    argv[n++] = "--loops";   argv[n++] = sloops;
    argv[n++] = "--workers"; argv[n++] = sworkers;
    for (int i = 0; extra && extra[i] && n < 30; i++) argv[n++] = (char *)extra[i];
    argv[n] = NULL;

    fflush(stdout);
    s->pid = fork();
    if (s->pid == 0) {
        setenv("BANK_NO_FSYNC", "1", 1);
        if (crash_every > 0) {
            char v[16];
            snprintf(v, sizeof v, "%d", crash_every);
            setenv("BANK_TEST_CRASH_EVERY", v, 1);
        }
        if (!getenv("SHOW_SERVER_LOG") && !freopen("/dev/null", "w", stderr)) _exit(126);   /* quiet unless SHOW_SERVER_LOG=1 */
        execv(argv[0], argv);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {                  /* wait up to 10 s */
        if (waitpid(s->pid, NULL, WNOHANG) == s->pid) return -2;     /* it died: bad port? */
        BankConn c;
        if (cl_connect(&c, "127.0.0.1", s->port) == 0) {
            int st = cl_login(&c, "admin", "admin123", NULL);
            cl_close(&c);
            if (st == 0) return 0;
        }
        usleep(100000);
    }
    return -1;
}

/* Start bin/bank_server (Srv must start zeroed: Srv s = {0}).
 * `extra` = NULL-terminated list of extra arguments, e.g. {"--idle","2",NULL}.
 * `crash_every` > 0 sets BANK_TEST_CRASH_EVERY.  A non-empty s->dir is reused
 * (restart on the same data).  Ports are picked BELOW the range Linux uses for
 * outgoing connections (32768+), and another port is tried if one is taken.
 * Returns 0 when the server answers a login, -1 otherwise. */
static inline int srv_start(Srv *s, int loops, int workers, const char *const *extra, int crash_every)
{
    static int counter = 0;
    if (s->dir[0] == '\0') make_test_dir(s->dir);
    for (int attempt = 0; attempt < 8; attempt++) {
        s->port = 20000 + (int)(((unsigned)getpid() * 37u + (unsigned)(counter++) * 1013u + (unsigned)time(NULL) * 17u) % 11000u);
        int r = srv_try(s, loops, workers, extra, crash_every);
        if (r != -2) return r;
    }
    return -1;
}

/* SIGTERM the supervisor and wait for it.  Returns its exit code (or -1). */
static inline int srv_stop(Srv *s)
{
    int status = 0, code = -1;
    kill(s->pid, SIGTERM);
    for (int i = 0; i < 200; i++) {                  /* up to 20 s */
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) { code = WIFEXITED(status) ? WEXITSTATUS(status) : -2; break; }
        usleep(100000);
    }
    if (code == -1) { kill(s->pid, SIGKILL); waitpid(s->pid, &status, 0); }
    return code;
}

static inline void srv_cleanup(Srv *s) { remove_test_dir(s->dir); }

/* ---------- looking at processes through /proc ---------- */

/* Parse /proc/PID/stat.  Returns 0 if the process does not exist.
 * *state = 'R','S','Z',...   *ppid = parent. */
static inline int proc_stat(pid_t pid, char *state, pid_t *ppid)
{
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');                     /* the name may contain spaces */
    if (!p) return 0;
    char st; int pp;
    if (sscanf(p + 2, "%c %d", &st, &pp) != 2) return 0;
    if (state) *state = st;
    if (ppid) *ppid = pp;
    return 1;
}

/* Is the process really running?  (A zombie counts as dead.) */
static inline int proc_alive(pid_t pid)
{
    char st;
    return proc_stat(pid, &st, NULL) && st != 'Z';
}

/* Direct children of `parent` (not zombies). Returns how many. */
static inline int proc_children(pid_t parent, pid_t *out, int max)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        pid_t pid = atoi(e->d_name);
        char st; pid_t pp;
        if (pid <= 0) continue;
        if (proc_stat(pid, &st, &pp) && pp == parent && st != 'Z') out[n++] = pid;
    }
    closedir(d);
    return n;
}

/* All workers of the whole server (children of children of the supervisor). */
static inline int srv_workers(Srv *s, pid_t *out, int max)
{
    pid_t loops[64];
    int nl = proc_children(s->pid, loops, 64), n = 0;
    for (int i = 0; i < nl; i++) n += proc_children(loops[i], out + n, max - n);
    return n;
}

/* Wait (up to timeout_ms) until the server has `want` workers. */
static inline int srv_wait_workers(Srv *s, int want, int timeout_ms)
{
    pid_t w[256];
    long end = now_ms_t() + timeout_ms;
    while (now_ms_t() < end) {
        if (srv_workers(s, w, 256) >= want) return 1;
        usleep(50000);
    }
    return 0;
}

/* ---------- connection helpers ---------- */

static inline void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max > 1048576 ? 1048576 : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* Connect and log in; returns 0 or the failing status. */
static inline int conn_login(BankConn *c, int port, const char *user, const char *pass)
{
    if (cl_connect(c, "127.0.0.1", port) < 0) return CL_E_IO;
    int st = cl_login(c, user, pass, NULL);
    if (st != 0) cl_close(c);
    return st;
}

/* Has the server closed this socket? (waits up to timeout_ms) */
static inline int peer_closed(int fd, int timeout_ms)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    char b[512];
    long end = now_ms_t() + timeout_ms;
    for (;;) {
        long left = end - now_ms_t();
        if (left < 0) return 0;
        if (poll(&pf, 1, (int)left) <= 0) return 0;
        ssize_t n = read(fd, b, sizeof b);
        if (n == 0) return 1;
        if (n < 0 && errno != EINTR && errno != EAGAIN) return 1;
    }
}

/* create a customer through the protocol; returns account id or -1 */
static inline int add_customer(BankConn *emp, const char *user, const char *name, int64_t initial)
{
    ReqAddUser in;
    Packet rp;
    RespIds r;
    memset(&in, 0, sizeof in);
    snprintf(in.username, sizeof in.username, "%s", user);
    snprintf(in.password, sizeof in.password, "%s", "pw");
    snprintf(in.name, sizeof in.name, "%s", name);
    in.initial = initial;
    if (cl_call(emp, OP_ADD_CUSTOMER, &in, sizeof in, &rp) != 0) return -1;
    PAYLOAD_GET(&rp, r);
    return r.id2;
}

static inline int add_staff(BankConn *admin, const char *user, int role)
{
    ReqAddUser in;
    Packet rp;
    RespIds r;
    memset(&in, 0, sizeof in);
    snprintf(in.username, sizeof in.username, "%s", user);
    snprintf(in.password, sizeof in.password, "%s", "pw");
    snprintf(in.name, sizeof in.name, "%s", user);
    in.role = role;
    if (cl_call(admin, OP_ADD_STAFF, &in, sizeof in, &rp) != 0) return -1;
    PAYLOAD_GET(&rp, r);
    return r.id;
}

static inline int64_t conn_balance(BankConn *c)
{
    Packet rp;
    RespBalance r;
    if (cl_call(c, OP_BALANCE, NULL, 0, &rp) != 0) return -1;
    PAYLOAD_GET(&rp, r);
    return r.balance;
}

static inline int conn_deposit(BankConn *c, int64_t amount, int64_t *newbal)
{
    ReqAmount in = { amount };
    Packet rp;
    RespBalance r;
    int st = cl_call(c, OP_DEPOSIT, &in, sizeof in, &rp);
    if (st == 0 && newbal) { PAYLOAD_GET(&rp, r); *newbal = r.balance; }
    return st;
}

static inline int conn_transfer(BankConn *c, int to_account, int64_t amount)
{
    ReqTransfer in;
    memset(&in, 0, sizeof in);
    in.to_account = to_account;
    in.amount = amount;
    return cl_call(c, OP_TRANSFER, &in, sizeof in, NULL);
}

#endif
