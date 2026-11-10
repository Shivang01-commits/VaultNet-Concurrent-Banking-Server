/*
 * test_server_sessions.c - "one session per user" against a REAL server.
 *
 *   A. 20 processes try to log in as the SAME user at the same moment
 *      -> exactly one wins, 19 get "already logged in"
 *   B. logout frees the session at once; many refused attempts do not disturb the holder
 *   C. the rule holds for every role (customer, employee, manager, admin)
 *   D. different users are independent
 *   E. an idle (silent) client is disconnected and its session is freed
 *   F. a killed worker does NOT log the user out
 *   G. killing every event loop frees all sessions (kernel releases the locks)
 */
#include "server_helpers.h"

static Srv S;

static int try_login_status(const char *user, const char *pass)
{
    BankConn c;
    int st = conn_login(&c, S.port, user, pass);
    if (st == 0) { cl_logout(&c); cl_close(&c); }
    return st;
}

static void race_same_user(void)
{
    BankConn adm, emp;
    int fds[2], ok = 0, refused = 0, other = 0;
    pid_t kids[20];

    SCENARIO("A. 20 processes log in as the same user at the same moment");
    conn_login(&adm, S.port, "admin", "admin123");
    add_staff(&adm, "emp", ROLE_EMPLOYEE);
    conn_login(&emp, S.port, "emp", "pw");
    CHECK(add_customer(&emp, "racer", "Racer", 100) > 0, "customer 'racer' exists");
    cl_close(&emp); cl_close(&adm);
    usleep(300000);

    if (pipe(fds) < 0) die("pipe");
    fflush(stdout);
    for (int i = 0; i < 20; i++) {
        if ((kids[i] = fork()) == 0) {
            char b;
            close(fds[1]);
            if (read(fds[0], &b, 1) < 0) {}                 /* wait for the start signal (pipe closes) */
            BankConn c;
            int st = conn_login(&c, S.port, "racer", "pw");
            if (st == 0) usleep(1500000);                    /* the winner stays logged in */
            _exit(st == 0 ? 0 : st == BANK_E_ALREADY_LOGGED_IN ? 1 : 2);
        }
    }
    close(fds[0]);
    usleep(300000);
    close(fds[1]);                                           /* "go!" */
    for (int i = 0; i < 20; i++) {              /* wait only for OUR children (the server is a child too) */
        int status;
        waitpid(kids[i], &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 2;
        if (code == 0) ok++; else if (code == 1) refused++; else other++;
    }
    printf("      winners %d, refused %d, other %d\n", ok, refused, other);
    CHECK(ok == 1, "exactly ONE of the 20 logins succeeded");
    CHECK(refused == 19 && other == 0, "the other 19 were told 'already logged in'");
    usleep(300000);
    CHECK(try_login_status("racer", "pw") == 0, "after the winner left, the user can log in again");
}

static void logout_frees(void)
{
    BankConn a, b;
    SCENARIO("B. logout frees the session at once");
    CHECK(conn_login(&a, S.port, "racer", "pw") == 0, "first connection logs in");
    int refused = 0;
    for (int i = 0; i < 30; i++) if (try_login_status("racer", "pw") == BANK_E_ALREADY_LOGGED_IN) refused++;
    CHECK(refused == 30, "30 more attempts (spread over both loops) are all refused");
    CHECK(conn_balance(&a) == 100, "the first connection is not disturbed");
    CHECK(cl_logout(&a) == 0, "first connection logs out");
    CHECK(conn_login(&b, S.port, "racer", "pw") == 0, "another connection can log in immediately");
    cl_logout(&b);
    cl_close(&a); cl_close(&b);
    usleep(300000);
}

static void every_role(void)
{
    BankConn adm, mgr, first[4], second;
    const char *names[4] = { "racer", "emp", "mgr", "admin" };
    const char *pws[4]   = { "pw", "pw", "pw", "admin123" };

    SCENARIO("C. the rule holds for every role");
    conn_login(&adm, S.port, "admin", "admin123");
    add_staff(&adm, "mgr", ROLE_MANAGER);
    cl_logout(&adm); cl_close(&adm);
    usleep(300000);
    (void)mgr;
    for (int i = 0; i < 4; i++) {
        char msg[100];
        snprintf(msg, sizeof msg, "%s logs in", names[i]);
        CHECK(conn_login(&first[i], S.port, names[i], pws[i]) == 0, msg);
    }
    for (int i = 0; i < 4; i++) {
        char msg[100];
        snprintf(msg, sizeof msg, "second login of %s is refused", names[i]);
        CHECK(conn_login(&second, S.port, names[i], pws[i]) == BANK_E_ALREADY_LOGGED_IN, msg);
    }
    CHECK(cl_logout(&first[0]) == 0 && try_login_status("racer", "pw") == 0, "after logout of the customer only that user is free again");
    CHECK(conn_login(&second, S.port, "emp", "pw") == BANK_E_ALREADY_LOGGED_IN, "...the employee is still held");
    for (int i = 0; i < 4; i++) cl_close(&first[i]);
    usleep(300000);
}

static void users_independent(void)
{
    BankConn a, b, e, adm;
    SCENARIO("D. different users do not block each other");
    conn_login(&adm, S.port, "admin", "admin123");
    add_staff(&adm, "emp9", ROLE_EMPLOYEE);
    conn_login(&e, S.port, "emp9", "pw");
    int x = add_customer(&e, "ux", "UX", 10), y = add_customer(&e, "uy", "UY", 10);
    CHECK(x > 0 && y > 0, "two customers exist");
    CHECK(conn_login(&a, S.port, "ux", "pw") == 0 && conn_login(&b, S.port, "uy", "pw") == 0, "both are logged in at the same time");
    cl_close(&a); cl_close(&b); cl_close(&e); cl_close(&adm);
    usleep(300000);
}

static void idle_frees(void)
{
    BankConn a;
    SCENARIO("E. a silent client is disconnected and the session is freed (server --idle 3)");
    CHECK(conn_login(&a, S.port, "ux", "pw") == 0, "ux logs in and then says nothing");
    CHECK(try_login_status("ux", "pw") == BANK_E_ALREADY_LOGGED_IN, "meanwhile a second login is refused");
    CHECK(peer_closed(a.fd, 8000), "the server disconnects the silent client");
    CHECK(try_login_status("ux", "pw") == 0, "the session is free: ux can log in again");
    cl_close(&a);
}

static void worker_kill_keeps_session(void)
{
    BankConn a;
    pid_t w[64];
    SCENARIO("F. killing workers does not log anybody out");
    CHECK(conn_login(&a, S.port, "uy", "pw") == 0, "uy logs in");
    int n = srv_workers(&S, w, 64);
    CHECK(n == 8, "8 workers are running");
    for (int i = 0; i < n; i++) kill(w[i], SIGKILL);
    CHECK(srv_wait_workers(&S, 8, 10000), "all workers were replaced");
    CHECK(conn_balance(&a) == 10, "uy's connection still works (still logged in)");
    CHECK(try_login_status("uy", "pw") == BANK_E_ALREADY_LOGGED_IN, "and a second login is still refused");
    cl_logout(&a); cl_close(&a);
    usleep(300000);
}

static void loop_kill_frees(void)
{
    BankConn a, b;
    pid_t loops[16];
    SCENARIO("G. killing every event loop frees all sessions");
    CHECK(conn_login(&a, S.port, "ux", "pw") == 0 && conn_login(&b, S.port, "uy", "pw") == 0, "ux and uy are logged in");
    int n = proc_children(S.pid, loops, 16);
    CHECK(n == 2, "2 loops are running");
    for (int i = 0; i < n; i++) kill(loops[i], SIGKILL);
    CHECK(peer_closed(a.fd, 5000) && peer_closed(b.fd, 5000), "both clients were disconnected");
    int again = 0;
    for (int i = 0; i < 100 && !again; i++) {
        usleep(100000);
        again = try_login_status("ux", "pw") == 0 && try_login_status("uy", "pw") == 0;
    }
    CHECK(again, "after the supervisor restarted the loops, both users can log in (no stale session)");
    cl_close(&a); cl_close(&b);
}

int main(void)
{
    printf("SESSION TESTS: one session per user, against a real server\n");
    raise_nofile();
    signal(SIGPIPE, SIG_IGN);
    const char *extra[] = { "--idle", "3", NULL };
    if (srv_start(&S, 2, 4, extra, 0) != 0) die("could not start bin/bank_server (run from the project folder)");

    race_same_user();
    logout_frees();
    every_role();
    users_independent();
    idle_frees();
    worker_kill_keeps_session();
    loop_kill_frees();

    CHECK(srv_stop(&S) == 0, "server stops cleanly");
    srv_cleanup(&S);
    return test_finish("test_server_sessions");
}
