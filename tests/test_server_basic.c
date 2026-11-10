/*
 * test_server_basic.c - starts a real bank_server and talks to it over TCP.
 *
 *   A. login errors and "log in first"
 *   B. every role end to end (admin, employee, manager, customer)
 *   C. paged lists
 *   D. one session per user (same loop and different loops), freed on disconnect
 *   E. protocol robustness: byte-by-byte packets, pipelined requests, garbage
 *   F. idle timeout
 *   G. many connections at once + a small throughput number
 */
#include "server_helpers.h"

static Srv S;

static void errors_and_login(void)
{
    BankConn c;
    Packet rp;
    SCENARIO("login errors");
    cl_connect(&c, "127.0.0.1", S.port);
    CHECK(cl_call(&c, OP_BALANCE, NULL, 0, &rp) == PROTO_E_NOT_LOGGED_IN, "request before login -> not logged in");
    CHECK(cl_login(&c, "admin", "wrong", NULL) == BANK_E_BAD_PASSWORD, "wrong password refused");
    CHECK(cl_login(&c, "nobody", "x", NULL) == BANK_E_BAD_PASSWORD, "unknown user gives the same error");
    CHECK(cl_login(&c, "admin", "admin123", NULL) == 0, "correct login works");
    CHECK(cl_login(&c, "admin", "admin123", NULL) == BANK_E_ALREADY_LOGGED_IN, "second login on the same connection refused");
    CHECK(cl_logout(&c) == 0, "logout works");
    CHECK(cl_call(&c, OP_BALANCE, NULL, 0, &rp) == PROTO_E_NOT_LOGGED_IN, "after logout: not logged in again");
    CHECK(cl_login(&c, "admin", "admin123", NULL) == 0, "the same connection can log in again");
    cl_close(&c);
}

static void all_roles(void)
{
    BankConn adm, emp, mgr, alice, bob;
    Packet rp;
    RespIds ids;
    int64_t nb;

    SCENARIO("all roles, end to end");
    CHECK(conn_login(&adm, S.port, "admin", "admin123") == 0, "admin logs in");
    int emp_id = add_staff(&adm, "emp", ROLE_EMPLOYEE);
    int mgr_id = add_staff(&adm, "mgr", ROLE_MANAGER);
    CHECK(emp_id > 0 && mgr_id > 0, "admin creates an employee and a manager");
    CHECK(conn_login(&emp, S.port, "emp", "pw") == 0, "employee logs in");
    CHECK(conn_login(&mgr, S.port, "mgr", "pw") == 0, "manager logs in");

    int a_acc = add_customer(&emp, "alice", "Alice", 1000);
    int b_acc = add_customer(&emp, "bob", "Bob", 500);
    CHECK(a_acc > 0 && b_acc > 0 && a_acc != b_acc, "employee creates two customers (with accounts)");
    CHECK(add_customer(&emp, "alice", "Again", 1) == -1, "duplicate username refused");
    CHECK(conn_login(&alice, S.port, "alice", "pw") == 0 && conn_login(&bob, S.port, "bob", "pw") == 0, "customers log in");

    CHECK(conn_balance(&alice) == 1000, "alice balance 1000");
    CHECK(conn_deposit(&alice, 200, &nb) == 0 && nb == 1200, "deposit 200 -> 1200");
    ReqAmount w = { 100 };
    CHECK(cl_call(&alice, OP_WITHDRAW, &w, sizeof w, &rp) == 0, "withdraw 100");
    ReqAmount big = { 999999 };
    CHECK(cl_call(&alice, OP_WITHDRAW, &big, sizeof big, NULL) == BANK_E_INSUFFICIENT, "withdraw too much -> insufficient funds");
    CHECK(conn_transfer(&alice, b_acc, 300) == 0, "transfer 300 to bob");
    CHECK(conn_balance(&alice) == 800 && conn_balance(&bob) == 800, "balances 800 / 800");
    CHECK(conn_transfer(&alice, 99999, 1) == BANK_E_NOT_FOUND, "transfer to a missing account refused");

    ReqText fb; memset(&fb, 0, sizeof fb); strcpy(fb.text, "great bank");
    CHECK(cl_call(&alice, OP_FEEDBACK_ADD, &fb, sizeof fb, NULL) == 0, "customer sends feedback");
    CHECK(cl_call(&alice, OP_LOAN_APPLY, &(ReqAmount){ 5000 }, sizeof(ReqAmount), &rp) == 0, "customer applies for a loan");
    PAYLOAD_GET(&rp, ids);
    int loan = ids.id;

    /* roles are enforced by the server */
    CHECK(cl_call(&alice, OP_PENDING_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), NULL) == BANK_E_PERMISSION, "customer cannot list pending loans");
    CHECK(cl_call(&alice, OP_FIND_USER, &(ReqFind){"bob"}, sizeof(ReqFind), NULL) == BANK_E_PERMISSION, "customer cannot search users");
    CHECK(cl_call(&emp, OP_DEPOSIT, &w, sizeof w, NULL) == BANK_E_PERMISSION, "employee has no account to deposit into");
    CHECK(cl_call(&emp, OP_SET_ROLE, &(ReqTwoInts){ mgr_id, ROLE_EMPLOYEE }, sizeof(ReqTwoInts), NULL) == BANK_E_PERMISSION, "employee cannot change roles");

    /* manager assigns, employee approves */
    CHECK(cl_call(&mgr, OP_PENDING_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 1, "manager sees 1 pending loan");
    CHECK(cl_call(&mgr, OP_ASSIGN_LOAN, &(ReqTwoInts){ loan, emp_id }, sizeof(ReqTwoInts), NULL) == 0, "manager assigns it to the employee");
    CHECK(cl_call(&emp, OP_ASSIGNED_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 1, "employee sees 1 assigned loan");
    CHECK(cl_call(&emp, OP_PROCESS_LOAN, &(ReqTwoInts){ loan, 1 }, sizeof(ReqTwoInts), NULL) == 0, "employee approves it");
    CHECK(conn_balance(&alice) == 5800, "loan amount was credited (800 + 5000)");
    CHECK(cl_call(&emp, OP_PROCESS_LOAN, &(ReqTwoInts){ loan, 1 }, sizeof(ReqTwoInts), NULL) == BANK_E_STATE, "a loan cannot be decided twice");
    CHECK(cl_call(&alice, OP_MY_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 1, "customer sees own loan");
    CHECK(cl_call(&mgr, OP_FEEDBACK_LIST, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 1, "manager reads the feedback");

    /* users */
    RespUser u;
    CHECK(cl_call(&emp, OP_FIND_USER, &(ReqFind){"bob"}, sizeof(ReqFind), &rp) == 0, "employee finds bob by name");
    PAYLOAD_GET(&rp, u);
    CHECK(u.account_id == b_acc && u.role == ROLE_CUSTOMER, "found record is right");
    ReqModify mod; memset(&mod, 0, sizeof mod); mod.user_id = u.id; strcpy(mod.name, "Robert");
    CHECK(cl_call(&emp, OP_MODIFY_USER, &mod, sizeof mod, NULL) == 0, "employee renames bob");
    CHECK(cl_call(&emp, OP_GET_USER, &(ReqTwoInts){ u.id, 0 }, sizeof(ReqTwoInts), &rp) == 0, "get user by id");
    PAYLOAD_GET(&rp, u);
    CHECK(strcmp(u.name, "Robert") == 0, "new name is stored");

    /* freezing an account */
    CHECK(cl_call(&mgr, OP_SET_ACCOUNT_ACTIVE, &(ReqTwoInts){ b_acc, 0 }, sizeof(ReqTwoInts), NULL) == 0, "manager freezes bob's account");
    CHECK(conn_deposit(&bob, 1, NULL) == BANK_E_INACTIVE, "frozen account refuses deposits");
    CHECK(cl_call(&mgr, OP_SET_ACCOUNT_ACTIVE, &(ReqTwoInts){ b_acc, 1 }, sizeof(ReqTwoInts), NULL) == 0, "manager unfreezes it");
    CHECK(conn_deposit(&bob, 1, NULL) == 0, "deposits work again");

    /* password change */
    ReqText np; memset(&np, 0, sizeof np); strcpy(np.text, "newpw");
    CHECK(cl_call(&bob, OP_PASSWD, &np, sizeof np, NULL) == 0, "bob changes his password");
    cl_close(&bob);
    usleep(300000);
    CHECK(conn_login(&bob, S.port, "bob", "pw") == BANK_E_BAD_PASSWORD, "old password no longer works");
    CHECK(conn_login(&bob, S.port, "bob", "newpw") == 0, "new password works");

    /* role change takes effect on the very next request of a logged-in user */
    CHECK(cl_call(&adm, OP_SET_ROLE, &(ReqTwoInts){ emp_id, ROLE_MANAGER }, sizeof(ReqTwoInts), NULL) == 0, "admin turns the employee into a manager");
    CHECK(cl_call(&emp, OP_PENDING_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), NULL) == 0, "...and the logged-in connection now has manager rights at once");

    cl_close(&adm); cl_close(&emp); cl_close(&mgr); cl_close(&alice); cl_close(&bob);
}


/* ---- remaining lines of the requirements: reject loan, every role changes its own
 *      password, admin edits customer + employee, passbook permissions, logout ---- */
static int user_id_of(BankConn *c, const char *name)
{
    Packet rp;
    RespUser u;
    ReqFind f; memset(&f, 0, sizeof f); snprintf(f.username, sizeof f.username, "%s", name);
    if (cl_call(c, OP_FIND_USER, &f, sizeof f, &rp) != 0) return -1;
    PAYLOAD_GET(&rp, u);
    return u.id;
}

/* change the password of `user` through the server and prove old fails / new works */
static void passwd_roundtrip(const char *user, const char *oldpw, const char *newpw, const char *what)
{
    BankConn c;
    char msg[120];
    ReqText np;
    memset(&np, 0, sizeof np);
    snprintf(np.text, sizeof np.text, "%s", newpw);
    if (conn_login(&c, S.port, user, oldpw) != 0) { CHECK(0, "login before password change"); return; }
    snprintf(msg, sizeof msg, "%s changes own password", what);
    CHECK(cl_call(&c, OP_PASSWD, &np, sizeof np, NULL) == 0, msg);
    CHECK(cl_logout(&c) == 0, "logout");
    cl_close(&c);
    snprintf(msg, sizeof msg, "%s: old password refused", what);
    CHECK(conn_login(&c, S.port, user, oldpw) == BANK_E_BAD_PASSWORD, msg);
    snprintf(msg, sizeof msg, "%s: new password accepted", what);
    CHECK(conn_login(&c, S.port, user, newpw) == 0, msg);
    cl_close(&c);
    usleep(300000);                                  /* let the server free the session */
}

static void requirement_gaps(void)
{
    BankConn adm, emp, mgr, carol, dave;
    Packet rp;
    RespIds ids;
    RespUser u;

    SCENARIO("requirement lines: reject loan, passwords of every role, admin edits, passbook rights");
    CHECK(conn_login(&adm, S.port, "admin", "admin123") == 0, "admin logs in");
    int emp_id = add_staff(&adm, "reqemp", ROLE_EMPLOYEE);
    int mgr_id = add_staff(&adm, "reqmgr", ROLE_MANAGER);
    CHECK(emp_id > 0 && mgr_id > 0, "admin adds an employee and a manager");
    CHECK(conn_login(&emp, S.port, "reqemp", "pw") == 0 && conn_login(&mgr, S.port, "reqmgr", "pw") == 0, "both log in");
    int c_acc = add_customer(&emp, "carol", "Carol", 1000);
    int d_acc = add_customer(&emp, "dave", "Dave", 1000);
    CHECK(c_acc > 0 && d_acc > 0, "employee opens two accounts");
    CHECK(conn_login(&carol, S.port, "carol", "pw") == 0 && conn_login(&dave, S.port, "dave", "pw") == 0, "customers log in");

    /* reject a loan */
    CHECK(cl_call(&carol, OP_LOAN_APPLY, &(ReqAmount){ 400 }, sizeof(ReqAmount), &rp) == 0, "carol applies for a loan of 400");
    PAYLOAD_GET(&rp, ids);
    int loan = ids.id;
    CHECK(cl_call(&mgr, OP_ASSIGN_LOAN, &(ReqTwoInts){ loan, emp_id }, sizeof(ReqTwoInts), NULL) == 0, "manager assigns it");
    CHECK(cl_call(&mgr, OP_PROCESS_LOAN, &(ReqTwoInts){ loan, 1 }, sizeof(ReqTwoInts), NULL) == BANK_E_PERMISSION, "a manager cannot approve loans");
    CHECK(cl_call(&emp, OP_PROCESS_LOAN, &(ReqTwoInts){ loan, 0 }, sizeof(ReqTwoInts), NULL) == 0, "employee REJECTS the loan");
    CHECK(conn_balance(&carol) == 1000, "rejected loan: no money credited");
    CHECK(cl_call(&carol, OP_MY_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 1, "carol sees her loan");
    Loan l; memcpy(&l, rp.payload, sizeof l);
    CHECK(l.status == LOAN_REJECTED, "its status is REJECTED");
    CHECK(cl_call(&emp, OP_PROCESS_LOAN, &(ReqTwoInts){ loan, 1 }, sizeof(ReqTwoInts), NULL) == BANK_E_STATE, "a rejected loan cannot be approved afterwards");
    CHECK(cl_call(&emp, OP_ASSIGNED_LOANS, &(ReqPage){0,0}, sizeof(ReqPage), &rp) == 0 && rp.count == 0, "it is no longer in the employee's assigned list");

    /* passbook rights */
    CHECK(conn_deposit(&carol, 50, NULL) == 0, "carol deposits 50");
    ReqPage pg = { c_acc, 0 };
    CHECK(cl_call(&emp, OP_HISTORY, &pg, sizeof pg, &rp) == 0 && rp.count == 2, "employee reads carol's passbook (opening + deposit)");
    pg.id = d_acc;
    CHECK(cl_call(&carol, OP_HISTORY, &pg, sizeof pg, NULL) == BANK_E_PERMISSION, "a customer cannot read someone else's passbook");
    CHECK(cl_call(&mgr, OP_HISTORY, &pg, sizeof pg, &rp) == 0, "a manager may read a passbook");

    /* admin edits a customer and an employee; others may not edit staff */
    ReqModify mod; memset(&mod, 0, sizeof mod);
    mod.user_id = user_id_of(&adm, "carol");
    snprintf(mod.name, sizeof mod.name, "%s", "Carol Admin-Edited");
    CHECK(cl_call(&adm, OP_MODIFY_USER, &mod, sizeof mod, NULL) == 0, "admin edits a customer");
    mod.user_id = emp_id;
    snprintf(mod.name, sizeof mod.name, "%s", "Employee Edited");
    CHECK(cl_call(&adm, OP_MODIFY_USER, &mod, sizeof mod, NULL) == 0, "admin edits an employee");
    CHECK(cl_call(&adm, OP_GET_USER, &(ReqTwoInts){ emp_id, 0 }, sizeof(ReqTwoInts), &rp) == 0, "admin reads the employee back");
    PAYLOAD_GET(&rp, u);
    CHECK(strcmp(u.name, "Employee Edited") == 0, "the employee's new name is stored");
    mod.user_id = mgr_id;
    CHECK(cl_call(&emp, OP_MODIFY_USER, &mod, sizeof mod, NULL) == BANK_E_PERMISSION, "an employee cannot edit staff");
    CHECK(cl_call(&mgr, OP_MODIFY_USER, &mod, sizeof mod, NULL) == BANK_E_PERMISSION, "a manager cannot edit users");

    /* logout works for every role (then Exit = closing the connection) */
    CHECK(cl_logout(&carol) == 0 && cl_logout(&dave) == 0 && cl_logout(&emp) == 0 && cl_logout(&mgr) == 0 && cl_logout(&adm) == 0,
          "logout works for customer, employee, manager and admin");
    cl_close(&carol); cl_close(&dave); cl_close(&emp); cl_close(&mgr); cl_close(&adm);
    usleep(300000);

    /* every role changes its own password */
    passwd_roundtrip("carol", "pw", "pw2", "customer");
    passwd_roundtrip("reqemp",  "pw", "pw2", "employee");
    passwd_roundtrip("reqmgr",  "pw", "pw2", "manager");
    passwd_roundtrip("admin", "admin123", "admin456", "administrator");
    passwd_roundtrip("admin", "admin456", "admin123", "administrator (changed back)");
}

static void paging(void)
{
    BankConn c;
    Packet rp;
    SCENARIO("paged passbook over the network (30 deposits)");
    CHECK(conn_login(&c, S.port, "alice", "pw") == 0, "alice logs in");
    int64_t before_lines = 0;
    { int after = 0, more; do { ReqPage q = { 0, after }; cl_call(&c, OP_HISTORY, &q, sizeof q, &rp);
        before_lines += rp.count; if (rp.count) { TxnRec t; memcpy(&t, rp.payload + (rp.count-1)*sizeof t, sizeof t); after = t.id; }
        more = rp.flags & FLAG_MORE; } while (more); }
    for (int i = 0; i < 30; i++) conn_deposit(&c, 1, NULL);

    int after = 0, lines = 0, pages = 0, last = 1 << 30, ordered = 1, more;
    do {
        ReqPage q = { 0, after };
        if (cl_call(&c, OP_HISTORY, &q, sizeof q, &rp) != 0) break;
        for (int i = 0; i < rp.count; i++) {
            TxnRec t; memcpy(&t, rp.payload + i * sizeof t, sizeof t);
            if (t.id >= last) ordered = 0;
            last = after = t.id;
        }
        lines += rp.count; pages++;
        more = rp.flags & FLAG_MORE;
    } while (more);
    CHECK(lines == before_lines + 30, "all lines arrive over several pages");
    CHECK(pages >= 3 && ordered, "split in pages, newest first");
    cl_close(&c);
}

static void one_session(void)
{
    BankConn a, b, c3;
    SCENARIO("one session per user (works across loops and inside one loop)");
    /* The supervisor gives each new connection to the least busy loop, so with
     * 2 loops: a -> loop 0, b -> loop 1, c3 -> loop 0 (same loop as a). */
    CHECK(conn_login(&a, S.port, "alice", "pw") == 0, "alice logs in on connection A");
    CHECK(conn_login(&b, S.port, "alice", "pw") == BANK_E_ALREADY_LOGGED_IN, "second connection (other loop) is refused");
    CHECK(conn_login(&c3, S.port, "alice", "pw") == BANK_E_ALREADY_LOGGED_IN, "third connection (same loop as A) is refused too");
    cl_close(&a);                                    /* the client simply disconnects */
    int ok = 0;
    for (int i = 0; i < 40 && !ok; i++) { usleep(50000); ok = conn_login(&b, S.port, "alice", "pw") == 0; }
    CHECK(ok, "after A disconnects (no logout) the session is free again");
    cl_close(&b); cl_close(&c3);
}

static void robustness(void)
{
    BankConn c;
    Packet rq, rp;
    SCENARIO("protocol robustness");
    CHECK(conn_login(&c, S.port, "alice", "pw") == 0, "alice logs in");

    /* 1. a request that arrives one byte at a time */
    memset(&rq, 0, sizeof rq);
    rq.magic = PROTO_MAGIC; rq.op = OP_BALANCE; rq.seq = 777;
    for (int i = 0; i < (int)sizeof rq; i++) {
        if (write(c.fd, (char *)&rq + i, 1) != 1) break;
        if (i < 40) usleep(1500);
    }
    CHECK(cl_recv_packet(&c, &rp, 5000) == 0 && rp.seq == 777 && rp.status == 0, "byte-by-byte request is answered correctly");

    /* 2. three requests sent in ONE write: replies come back in order */
    Packet three[3];
    memset(three, 0, sizeof three);
    for (int i = 0; i < 3; i++) { three[i].magic = PROTO_MAGIC; three[i].seq = 100 + i; }
    three[0].op = OP_BALANCE; three[1].op = OP_DEPOSIT; three[2].op = OP_BALANCE;
    ReqAmount one = { 1 };
    memcpy(three[1].payload, &one, sizeof one);
    if (write(c.fd, three, sizeof three) != (ssize_t)sizeof three) die("write");
    Packet r[3];
    int got = 0;
    for (int i = 0; i < 3; i++) if (cl_recv_packet(&c, &r[i], 5000) == 0) got++;
    CHECK(got == 3, "3 pipelined requests -> 3 replies");
    RespBalance b0, b2;
    memcpy(&b0, r[0].payload, sizeof b0); memcpy(&b2, r[2].payload, sizeof b2);
    CHECK(r[0].seq == 100 && r[1].seq == 101 && r[2].seq == 102, "replies are in the same order");
    CHECK(b2.balance == b0.balance + 1, "they were executed in order (deposit between the two balances)");

    /* 3. an unknown operation code closes the connection */
    memset(&rq, 0, sizeof rq);
    rq.magic = PROTO_MAGIC; rq.op = 200; rq.seq = 5;
    cl_send_packet(&c, &rq);
    CHECK(peer_closed(c.fd, 3000), "unknown operation -> server closes the connection");
    cl_close(&c);

    /* 4. garbage */
    cl_connect(&c, "127.0.0.1", S.port);
    memset(&rq, 0x55, sizeof rq);
    cl_send_packet(&c, &rq);
    CHECK(peer_closed(c.fd, 3000), "garbage (wrong magic) -> connection closed");
    cl_close(&c);

    /* 5. the server is still fine after all that */
    CHECK(conn_login(&c, S.port, "bob", "newpw") == 0 && conn_balance(&c) > 0, "server still works");
    cl_close(&c);
}

static void idle_timeout(void)
{
    Srv t;
    BankConn quiet, busy;
    const char *extra[] = { "--idle", "2", NULL };
    SCENARIO("idle timeout (second server with --idle 2)");
    if (srv_start(&t, 1, 2, extra, 0) != 0) { CHECK(0, "second server starts"); return; }
    cl_connect(&quiet, "127.0.0.1", t.port);
    conn_login(&busy, t.port, "admin", "admin123");
    int still = 1;
    for (int i = 0; i < 5; i++) {                    /* 5 s: busy talks every second */
        sleep(1);
        if (conn_balance(&busy) == -1 && cl_call(&busy, OP_GET_USER, &(ReqTwoInts){1,0}, sizeof(ReqTwoInts), NULL) != 0) still = 0;
    }
    CHECK(peer_closed(quiet.fd, 500), "a silent connection was disconnected after ~2 s");
    CHECK(still, "a connection that keeps talking was NOT disconnected");
    cl_close(&quiet); cl_close(&busy);
    CHECK(srv_stop(&t) == 0, "second server stops cleanly");
    srv_cleanup(&t);
}

static void many_connections(void)
{
    SCENARIO("many connections at once");
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    int logged = 300, idle = 1500;
    if (rl.rlim_cur < 4500) { logged = 100; idle = (int)rl.rlim_cur / 2 - 100; if (idle < 100) idle = 100; printf("    (low fd limit %ld: using %d logged-in + %d idle)\n", (long)rl.rlim_cur, logged, idle); }

    BankConn emp;
    conn_login(&emp, S.port, "emp", "pw");           /* emp is a manager now but can still add? no: use admin */
    cl_close(&emp);
    BankConn adm;
    conn_login(&adm, S.port, "admin", "admin123");
    /* create `logged` customers using an employee account */
    add_staff(&adm, "emp2", ROLE_EMPLOYEE);
    BankConn e2;
    conn_login(&e2, S.port, "emp2", "pw");
    int created = 0;
    for (int i = 0; i < logged; i++) {
        char n[32]; snprintf(n, sizeof n, "m%d", i);
        if (add_customer(&e2, n, n, 1000) > 0) created++;
    }
    CHECK(created == logged, "created the customers");

    BankConn *cs = calloc(logged, sizeof *cs);
    BankConn *ic = calloc(idle, sizeof *ic);
    int ok = 0, iok = 0;
    for (int i = 0; i < idle; i++) if (cl_connect(&ic[i], "127.0.0.1", S.port) == 0) iok++;
    for (int i = 0; i < logged; i++) {
        char n[32]; snprintf(n, sizeof n, "m%d", i);
        if (conn_login(&cs[i], S.port, n, "pw") == 0) ok++;
    }
    CHECK(iok == idle, "all idle connections accepted");
    CHECK(ok == logged, "all logged-in connections accepted and logged in");

    /* throughput: every connection asks for its balance, 30 rounds */
    int rounds = 30, good = 0;
    long t0 = now_ms_t();
    for (int r = 0; r < rounds; r++) {
        Packet rq;
        memset(&rq, 0, sizeof rq);
        rq.magic = PROTO_MAGIC; rq.op = OP_BALANCE;
        for (int i = 0; i < logged; i++) { rq.seq = r; cl_send_packet(&cs[i], &rq); }
        for (int i = 0; i < logged; i++) {
            Packet rp;
            if (cl_recv_packet(&cs[i], &rp, 10000) == 0 && rp.status == 0 && rp.seq == r) good++;
        }
    }
    long ms = now_ms_t() - t0;
    CHECK(good == rounds * logged, "every request of every connection got a correct reply");
    printf("    throughput: %d requests in %ld ms = %ld requests/second (%d clients, plus %d idle connections)\n",
           good, ms, ms ? good * 1000L / ms : 0, logged, idle);

    for (int i = 0; i < logged; i++) cl_close(&cs[i]);
    for (int i = 0; i < idle; i++) cl_close(&ic[i]);
    free(cs); free(ic);
    cl_close(&e2); cl_close(&adm);
}

int main(void)
{
    printf("SERVER TESTS (basic): real server process, real TCP connections\n");
    raise_nofile();
    signal(SIGPIPE, SIG_IGN);
    if (srv_start(&S, 2, 4, NULL, 0) != 0) die("could not start bin/bank_server (run from the project folder)");

    errors_and_login();
    all_roles();
    requirement_gaps();
    paging();
    one_session();
    robustness();
    many_connections();
    CHECK(srv_stop(&S) == 0, "server stops cleanly on SIGTERM (exit code 0)");
    srv_cleanup(&S);
    idle_timeout();
    return test_finish("test_server_basic");
}
