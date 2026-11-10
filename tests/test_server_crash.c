/*
 * test_server_crash.c - what happens when server processes die?
 *
 * The invariants checked after every load test (directly on the data files):
 *   1. for every account:  sum of its passbook lines == its balance
 *   2. money moved by transfers: total "transfer out" == total "transfer in"
 *      (so no half-done transfer exists)
 *
 *   A. random workers are killed (kill -9) while 6 clients hammer the server
 *   B. workers crash INSIDE the commit (4 different crash points) at random
 *   C. a loop process is killed (twice) under load
 *   D. the supervisor is killed: loops and workers must clean themselves up
 *   E. watchdog: a frozen worker is killed and the client gets an answer
 *   F. a worker restart must not leak session locks (descriptor inheritance)
 */
#include <sys/mman.h>
#include "server_helpers.h"

#define NCUST 12

typedef struct { volatile int ok, unknown, busy, ioerr, other; } Stats;

/* ---------------- a bank with NCUST customers ---------------- */
static int prepare(Srv *s, int accts[])
{
    BankConn adm, emp;
    if (conn_login(&adm, s->port, "admin", "admin123") != 0) return -1;
    add_staff(&adm, "emp", ROLE_EMPLOYEE);
    if (conn_login(&emp, s->port, "emp", "pw") != 0) return -1;
    for (int i = 0; i < NCUST; i++) {
        char n[16]; snprintf(n, sizeof n, "u%d", i);
        accts[i] = add_customer(&emp, n, n, 10000);
        if (accts[i] < 0) return -1;
    }
    cl_close(&adm); cl_close(&emp);
    return 0;
}

/* ---------------- one load-generating client process ---------------- */
static void load_client(int port, int me, const int *accts, long end_ms, Stats *st)
{
    BankConn c;
    int connected = 0;
    char name[16];
    snprintf(name, sizeof name, "u%d", me);
    srand((unsigned)getpid());

    while (now_ms_t() < end_ms) {
        if (!connected) {
            if (conn_login(&c, port, name, "pw") == 0) connected = 1;
            else { usleep(30000); continue; }          /* maybe the old session is still being freed */
        }
        int op = rand() % 10, s;
        if (op < 7) {
            int to = accts[rand() % NCUST];
            if (to == accts[me]) continue;
            s = conn_transfer(&c, to, 1 + rand() % 10);
        } else if (op < 9) {
            s = conn_deposit(&c, 1 + rand() % 5, NULL);
        } else {
            s = cl_call(&c, OP_BALANCE, NULL, 0, NULL);
        }
        if (s == 0 || s == BANK_E_INSUFFICIENT) st->ok++;
        else if (s == PROTO_E_UNKNOWN_RESULT)    st->unknown++;
        else if (s == PROTO_E_SERVER_BUSY)       st->busy++;
        else if (s == CL_E_IO)                 { st->ioerr++; cl_close(&c); connected = 0; }
        else                                     st->other++;
    }
    cl_close(&c);
}

/* start `n` client processes; they run until end_ms */
static void start_clients(pid_t *pids, int n, int port, const int *accts, long end_ms, Stats *st)
{
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] == 0) { signal(SIGPIPE, SIG_IGN); load_client(port, i, accts, end_ms, &st[i]); _exit(0); }
    }
}
static void wait_clients(pid_t *pids, int n) { for (int i = 0; i < n; i++) waitpid(pids[i], NULL, 0); }

/* ---------------- invariants, read straight from the files ---------------- */
static int check_invariants(const char *dir, const char *label, long *lines_out)
{
    Db db;
    char msg[160];
    if (db_open(&db, dir) < 0) { CHECK(0, "database opens after the server stopped"); return 0; }

    int na = db_count(&db, F_ACCOUNTS), nt = db_count(&db, F_TXNS);
    int64_t *sum = calloc(na + 1, sizeof *sum);
    int64_t out_sum = 0, in_sum = 0;
    long lines = 0, out_n = 0, in_n = 0;
    for (int i = 1; i < nt; i++) {
        TxnRec t;
        db_read(&db, F_TXNS, i, &t);
        if (t.id == 0) continue;
        lines++;
        if (t.account_id <= 0 || t.account_id >= na) continue;
        switch (t.type) {
        case TX_DEPOSIT: case TX_LOAN_CREDIT: sum[t.account_id] += t.amount; break;
        case TX_WITHDRAW:                     sum[t.account_id] -= t.amount; break;
        case TX_TRANSFER_OUT: sum[t.account_id] -= t.amount; out_sum += t.amount; out_n++; break;
        case TX_TRANSFER_IN:  sum[t.account_id] += t.amount; in_sum  += t.amount; in_n++;  break;
        }
    }
    int bad = 0;
    for (int a = 1; a < na; a++) {
        Account acc;
        db_read(&db, F_ACCOUNTS, a, &acc);
        if (acc.id != 0 && acc.balance != sum[a]) bad++;
    }
    snprintf(msg, sizeof msg, "%s: every account's passbook adds up to its balance (%d accounts, %ld lines)", label, na - 1, lines);
    CHECK(bad == 0, msg);
    snprintf(msg, sizeof msg, "%s: no half-done transfer (out %lld = in %lld, %ld lines each)", label, (long long)out_sum, (long long)in_sum, out_n);
    CHECK(out_sum == in_sum && out_n == in_n, msg);
    if (lines_out) *lines_out = lines;
    free(sum);
    db_close(&db);
    return 1;
}

static void sum_stats(Stats *st, int n, Stats *tot)
{
    memset(tot, 0, sizeof *tot);
    for (int i = 0; i < n; i++) {
        tot->ok += st[i].ok; tot->unknown += st[i].unknown; tot->busy += st[i].busy;
        tot->ioerr += st[i].ioerr; tot->other += st[i].other;
    }
}

static Stats *new_stats(int n) { return mmap(NULL, sizeof(Stats) * n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); }

/* ================================================================== */

static void kill_workers_under_load(void)
{
    Srv s = {0};
    int accts[NCUST];
    pid_t cl[6], w[256];
    Stats *st = new_stats(6), tot;

    SCENARIO("A. random workers are killed (kill -9) while 6 clients work for 6 seconds");
    if (srv_start(&s, 2, 4, NULL, 0) != 0 || prepare(&s, accts) != 0) { CHECK(0, "server starts"); return; }
    long end = now_ms_t() + 6000;
    start_clients(cl, 6, s.port, accts, end, st);

    int killed = 0;
    while (now_ms_t() < end - 300) {
        int n = srv_workers(&s, w, 256);
        if (n > 0) { kill(w[rand() % n], SIGKILL); killed++; }
        usleep(70000);
    }
    wait_clients(cl, 6);
    sum_stats(st, 6, &tot);
    printf("    workers killed: %d | client results: ok %d, unknown-result %d, try-again %d, connection errors %d, other %d\n",
           killed, tot.ok, tot.unknown, tot.busy, tot.ioerr, tot.other);
    CHECK(tot.ok > 200, "clients kept making progress during the killing");
    CHECK(tot.ioerr == 0, "no client lost its connection (a worker crash only affects one request)");
    CHECK(srv_wait_workers(&s, 8, 6000), "all 8 workers were replaced");
    BankConn c;
    CHECK(conn_login(&c, s.port, "u0", "pw") == 0 && conn_balance(&c) >= 0, "the server still answers");
    cl_close(&c);
    CHECK(srv_stop(&s) == 0, "clean stop");
    check_invariants(s.dir, "after worker kills", NULL);
    srv_cleanup(&s);
}

static void crash_inside_commit(void)
{
    Srv s = {0};
    int accts[NCUST];
    pid_t cl[6];
    Stats *st = new_stats(6), tot;

    SCENARIO("B. workers die INSIDE a commit at random (crash points 1-4), 1 request in 12");
    /* set the bank up with a normal server first, then restart on the same
     * data folder with crash injection switched on */
    if (srv_start(&s, 2, 4, NULL, 0) != 0 || prepare(&s, accts) != 0) { CHECK(0, "server starts"); return; }
    srv_stop(&s);
    if (srv_start(&s, 2, 4, NULL, 12) != 0) { CHECK(0, "server restarts with crash injection"); return; }
    long end = now_ms_t() + 6000;
    start_clients(cl, 6, s.port, accts, end, st);
    wait_clients(cl, 6);
    sum_stats(st, 6, &tot);
    printf("    client results: ok %d, unknown-result %d, try-again %d, connection errors %d, other %d\n",
           tot.ok, tot.unknown, tot.busy, tot.ioerr, tot.other);
    CHECK(tot.unknown > 5, "many requests ended with \"unknown result\" (workers really crashed inside commits)");
    CHECK(tot.ok > 100, "clients still made progress");
    CHECK(tot.ioerr == 0, "no client lost its connection");
    CHECK(tot.other == 0, "no unexpected error codes");
    CHECK(srv_stop(&s) == 0, "clean stop");
    check_invariants(s.dir, "after crashes inside commits", NULL);
    srv_cleanup(&s);
}

static void kill_loops(void)
{
    Srv s = {0};
    int accts[NCUST];
    pid_t cl[6], lp[8];
    Stats *st = new_stats(6), tot;

    SCENARIO("C. a loop process is killed twice while 6 clients work");
    if (srv_start(&s, 2, 3, NULL, 0) != 0 || prepare(&s, accts) != 0) { CHECK(0, "server starts"); return; }
    long end = now_ms_t() + 7000;
    start_clients(cl, 6, s.port, accts, end, st);

    for (int round = 0; round < 2; round++) {
        usleep(round == 0 ? 2000000 : 1500000);
        int n = proc_children(s.pid, lp, 8);
        if (n > 0) kill(lp[rand() % n], SIGKILL);
    }
    wait_clients(cl, 6);
    sum_stats(st, 6, &tot);
    printf("    client results: ok %d, unknown-result %d, try-again %d, connection errors %d, other %d\n",
           tot.ok, tot.unknown, tot.busy, tot.ioerr, tot.other);
    CHECK(tot.ioerr > 0, "clients of the dead loops were disconnected (and reconnected)");
    CHECK(tot.ok > 200, "work continued");
    int n = proc_children(s.pid, lp, 8);
    CHECK(n == 2, "the supervisor restarted the loop (2 loops running again)");
    CHECK(srv_wait_workers(&s, 6, 6000), "and its workers (6 workers again)");
    BankConn c;
    CHECK(conn_login(&c, s.port, "u1", "pw") == 0, "a user that was connected to a dead loop can log in again");
    cl_close(&c);
    CHECK(srv_stop(&s) == 0, "clean stop");
    check_invariants(s.dir, "after loop kills", NULL);
    srv_cleanup(&s);
}

static void kill_supervisor(void)
{
    Srv s = {0};
    pid_t loops[8], workers[256], all[300];
    int nall = 0;
    BankConn c;

    SCENARIO("D. the supervisor is killed (kill -9)");
    if (srv_start(&s, 2, 3, NULL, 0) != 0) { CHECK(0, "server starts"); return; }
    CHECK(conn_login(&c, s.port, "admin", "admin123") == 0, "a client is connected");
    int nl = proc_children(s.pid, loops, 8);
    for (int i = 0; i < nl; i++) all[nall++] = loops[i];
    int nw = srv_workers(&s, workers, 256);
    for (int i = 0; i < nw; i++) all[nall++] = workers[i];
    CHECK(nl == 2 && nw == 6, "2 loops and 6 workers are running");

    kill(s.pid, SIGKILL);
    waitpid(s.pid, NULL, 0);

    long end = now_ms_t() + 15000;
    int alive = nall;
    while (alive > 0 && now_ms_t() < end) {
        alive = 0;
        for (int i = 0; i < nall; i++) if (proc_alive(all[i])) alive++;
        if (alive) usleep(100000);
    }
    CHECK(alive == 0, "all loops and workers exited by themselves (no orphans left)");
    CHECK(peer_closed(c.fd, 2000), "the client connection was closed");
    cl_close(&c);

    /* the data survived: a new server on the same folder works */
    BankConn c2;
    Srv s2 = s;                                      /* same port, same data dir */
    char sport[16], sloops[] = "1", sworkers[] = "2";
    snprintf(sport, sizeof sport, "%d", s.port);
    pid_t p = fork();
    if (p == 0) {
        setenv("BANK_NO_FSYNC", "1", 1);
        if (!freopen("/dev/null", "w", stderr)) _exit(126);
        execl("bin/bank_server", "bank_server", "--port", sport, "--data", s.dir, "--loops", sloops, "--workers", sworkers, (char *)NULL);
        _exit(127);
    }
    s2.pid = p;
    int ok = 0;
    for (int i = 0; i < 60 && !ok; i++) { usleep(100000); ok = conn_login(&c2, s.port, "admin", "admin123") == 0; }
    CHECK(ok, "a new server started on the same data folder accepts the admin login");
    cl_close(&c2);
    srv_stop(&s2);
    srv_cleanup(&s);
}

static void watchdog(void)
{
    Srv s = {0};
    pid_t w[16];
    BankConn c;
    const char *extra[] = { "--worker-timeout", "2", NULL };

    SCENARIO("E. watchdog: a frozen worker (SIGSTOP) is killed after 2 s");
    if (srv_start(&s, 1, 2, extra, 0) != 0) { CHECK(0, "server starts"); return; }
    CHECK(conn_login(&c, s.port, "admin", "admin123") == 0, "admin logs in");
    int n = srv_workers(&s, w, 16);
    for (int i = 0; i < n; i++) kill(w[i], SIGSTOP);

    long t0 = now_ms_t();
    int st = cl_call_timeout(&c, OP_GET_USER, &(ReqTwoInts){ 1, 0 }, sizeof(ReqTwoInts), NULL, 15000);
    long took = now_ms_t() - t0;
    printf("    the request came back after %ld ms with status %d (%s)\n", took, st, cl_strerror(st));
    CHECK(took >= 1800 && took < 6000, "the watchdog acted after about 2 seconds");
    CHECK(st == PROTO_E_SERVER_BUSY, "read-only request: client is told \"try again\" (nothing could have happened)");

    int good = 0;
    for (int i = 0; i < 4 && !good; i++) {
        st = cl_call_timeout(&c, OP_GET_USER, &(ReqTwoInts){ 1, 0 }, sizeof(ReqTwoInts), NULL, 15000);
        good = (st == 0);
    }
    CHECK(good, "later requests are served by a fresh worker");
    CHECK(srv_stop(&s) == 0, "server stops cleanly even with a frozen worker left over");
    cl_close(&c);
    srv_cleanup(&s);
}

static void descriptor_leak(void)
{
    Srv s = {0};
    pid_t w[16];
    BankConn a, b, adm;
    int accts[NCUST];

    SCENARIO("F. restarting workers must not keep sessions alive (descriptor inheritance)");
    if (srv_start(&s, 1, 2, NULL, 0) != 0 || prepare(&s, accts) != 0) { CHECK(0, "server starts"); return; }
    CHECK(conn_login(&a, s.port, "u3", "pw") == 0, "u3 logs in (the loop now holds u3's session lock)");
    int n = srv_workers(&s, w, 16);
    for (int i = 0; i < n; i++) kill(w[i], SIGKILL);
    CHECK(srv_wait_workers(&s, 2, 5000), "both workers were restarted (forked by the loop that holds u3's lock)");
    CHECK(conn_balance(&a) == 10000, "u3's connection still works after the restart");
    cl_close(&a);
    int ok = 0;
    for (int i = 0; i < 40 && !ok; i++) { usleep(50000); ok = conn_login(&b, s.port, "u3", "pw") == 0; }
    CHECK(ok, "after u3 disconnects the session is free (the new workers did not inherit the lock)");
    cl_close(&b);
    (void)adm;
    CHECK(srv_stop(&s) == 0, "clean stop");
    srv_cleanup(&s);
}

/* ./bin/test_server_crash [ABCDEF]  runs only the listed scenarios */
static const char *g_only = NULL;
static int want(char letter) { return !g_only || strchr(g_only, letter); }

int main(int argc, char **argv)
{
    if (argc > 1) g_only = argv[1];
    printf("SERVER CRASH TESTS: processes are killed while the bank is busy\n");
    raise_nofile();
    signal(SIGPIPE, SIG_IGN);
    if (want('A')) kill_workers_under_load();
    if (want('B')) crash_inside_commit();
    if (want('C')) kill_loops();
    if (want('D')) kill_supervisor();
    if (want('E')) watchdog();
    if (want('F')) descriptor_leak();
    return test_finish("test_server_crash");
}
