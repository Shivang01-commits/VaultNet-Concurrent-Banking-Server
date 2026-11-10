/*
 * bank_cmd - typed-command client (for scripts and quick tests). The menu client is bank_client.
 *
 *   ./bin/bank_cmd [host] [port]        (default 127.0.0.1 9090)
 *
 * Commands (type "help" inside):
 *   login USER PASS | logout | quit
 *   balance | deposit N | withdraw N | transfer ACCOUNT N | loan N
 *   feedback TEXT | passwd NEWPASS | history [ACCOUNT] | myloans
 *   addcustomer USER PASS NAME INITIAL | modify USERID NAME | assigned | process LOAN 1|0
 *   user ID | find USERNAME
 *   freeze ACCOUNT 0|1 | assign LOAN EMPLOYEEID | pending | feedbacks
 *   addstaff USER PASS NAME employee|manager | setrole USERID employee|manager
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"

static BankConn conn;

static const char *role_name(int r)
{
    return r == ROLE_CUSTOMER ? "customer" : r == ROLE_EMPLOYEE ? "employee" :
           r == ROLE_MANAGER ? "manager" : r == ROLE_ADMIN ? "admin" : "?";
}
static const char *tx_name(int t)
{
    static const char *n[] = { "?", "deposit", "withdraw", "transfer-out", "transfer-in", "loan-credit" };
    return (t >= 1 && t <= 5) ? n[t] : "?";
}
static const char *loan_name(int s)
{
    static const char *n[] = { "pending", "assigned", "approved", "rejected" };
    return (s >= 0 && s <= 3) ? n[s] : "?";
}
static void fmt_time(int64_t t, char *buf, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* Do a call; print the error text if it failed.  Returns the status. */
static int call(int op, const void *p, size_t len, Packet *rep)
{
    int st = cl_call(&conn, op, p, len, rep);
    if (st == CL_E_IO) { printf("connection lost\n"); exit(1); }
    if (st != 0) printf("error %d: %s\n", st, proto_strerror(st));
    return st;
}

static void copy_str(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src); }

/* Fetch and print every page of a list. kind: 'h' history, 'l' loans, 'f' feedback */
static void list_all(int op, int id, char kind)
{
    ReqPage rq = { id, 0 };
    Packet rep;
    char tb[32];

    for (;;) {
        if (call(op, &rq, sizeof rq, &rep) != 0) return;
        int last = rq.after_id;
        for (int i = 0; i < rep.count; i++) {
            if (kind == 'h') {
                TxnRec r; memcpy(&r, rep.payload + i * sizeof r, sizeof r);
                fmt_time(r.time, tb, sizeof tb);
                printf("  #%d %s %-12s amount %lld  other-acct %d  balance %lld\n", r.id, tb,
                       tx_name(r.type), (long long)r.amount, r.other_account, (long long)r.balance_after);
                last = r.id;
            } else if (kind == 'l') {
                Loan l; memcpy(&l, rep.payload + i * sizeof l, sizeof l);
                printf("  loan #%d account %d customer %d amount %lld status %s employee %d\n", l.id,
                       l.account_id, l.customer_id, (long long)l.amount, loan_name(l.status), l.employee_id);
                last = l.id;
            } else {
                Feedback f; memcpy(&f, rep.payload + i * sizeof f, sizeof f);
                fmt_time(f.time, tb, sizeof tb);
                printf("  #%d %s %s: %.*s\n", f.id, f.username, tb, TEXT_LEN, f.text);
                last = f.id;
            }
        }
        if (!(rep.flags & FLAG_MORE)) break;
        rq.after_id = last;
    }
    printf("  (end of list)\n");
}

static void print_user(const Packet *rep)
{
    RespUser u; memcpy(&u, rep->payload, sizeof u);
    printf("  user %d '%s' (%s) role %s account %d active %d\n", u.id, u.username, u.name,
           role_name(u.role), u.account_id, u.active);
}

static int role_from(const char *s) { return !strcmp(s, "manager") ? ROLE_MANAGER : ROLE_EMPLOYEE; }

static void help(void)
{
    puts("login U P | logout | quit | balance | deposit N | withdraw N | transfer ACCT N | loan N\n"
         "feedback TEXT | passwd NEW | history [ACCT] | myloans\n"
         "addcustomer U P NAME INITIAL | modify USERID NAME | assigned | process LOAN 1|0 | user ID | find U\n"
         "freeze ACCT 0|1 | assign LOAN EMP | pending | feedbacks\n"
         "addstaff U P NAME employee|manager | setrole USERID employee|manager");
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 9090;
    char line[512];
    Packet rep;

    if (cl_connect(&conn, host, port) < 0) { fprintf(stderr, "cannot connect to %s:%d\n", host, port); return 1; }
    printf("connected. type 'help'.\n");

    while (fgets(line, sizeof line, stdin)) {
        char *w[6] = {0};
        int n = 0;
        line[strcspn(line, "\r\n")] = 0;
        char *rest = line;
        /* split into up to 5 words; the 6th "word" keeps the rest of the line (for TEXT / NAME) */
        for (; n < 5 && *rest; n++) {
            while (*rest == ' ') rest++;
            if (!*rest) break;
            w[n] = rest;
            while (*rest && *rest != ' ') rest++;
            if (*rest) *rest++ = 0;
        }
        while (*rest == ' ') rest++;
        w[n < 5 ? n : 5] = *rest ? rest : NULL;
        if (!w[0]) continue;
        const char *cmd = w[0];

        if (!strcmp(cmd, "quit")) break;
        else if (!strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "login") && w[1] && w[2]) {
            ReqLogin rq; memset(&rq, 0, sizeof rq);
            copy_str(rq.username, sizeof rq.username, w[1]);
            copy_str(rq.password, sizeof rq.password, w[2]);
            if (call(OP_LOGIN, &rq, sizeof rq, &rep) == 0) {
                RespLogin r; memcpy(&r, rep.payload, sizeof r);
                printf("  logged in: user %d role %s account %d\n", r.user_id, role_name(r.role), r.account_id);
            }
        }
        else if (!strcmp(cmd, "logout")) { if (call(OP_LOGOUT, NULL, 0, &rep) == 0) puts("  logged out"); }
        else if (!strcmp(cmd, "balance")) {
            if (call(OP_BALANCE, NULL, 0, &rep) == 0) { RespBalance b; memcpy(&b, rep.payload, sizeof b); printf("  balance = %lld\n", (long long)b.balance); }
        }
        else if ((!strcmp(cmd, "deposit") || !strcmp(cmd, "withdraw")) && w[1]) {
            ReqAmount a = { atoll(w[1]) };
            if (call(!strcmp(cmd, "deposit") ? OP_DEPOSIT : OP_WITHDRAW, &a, sizeof a, &rep) == 0) {
                RespBalance b; memcpy(&b, rep.payload, sizeof b); printf("  ok, new balance = %lld\n", (long long)b.balance); }
        }
        else if (!strcmp(cmd, "transfer") && w[1] && w[2]) {
            ReqTransfer t = { atoi(w[1]), 0, atoll(w[2]) };
            if (call(OP_TRANSFER, &t, sizeof t, &rep) == 0) {
                RespBalance b; memcpy(&b, rep.payload, sizeof b); printf("  ok, new balance = %lld\n", (long long)b.balance); }
        }
        else if (!strcmp(cmd, "loan") && w[1]) {
            ReqAmount a = { atoll(w[1]) };
            if (call(OP_LOAN_APPLY, &a, sizeof a, &rep) == 0) { RespIds r; memcpy(&r, rep.payload, sizeof r); printf("  loan applied, id %d\n", r.id); }
        }
        else if ((!strcmp(cmd, "feedback") || !strcmp(cmd, "passwd")) && w[1]) {
            /* rebuild the text from the original line (everything after the command) */
            const char *text = line + strlen(cmd) + 1;
            while (*text == ' ') text++;
            ReqText t; memset(&t, 0, sizeof t);
            copy_str(t.text, sizeof t.text, !strcmp(cmd, "passwd") ? w[1] : text);
            if (call(!strcmp(cmd, "feedback") ? OP_FEEDBACK_ADD : OP_PASSWD, &t, sizeof t, &rep) == 0) puts("  ok");
        }
        else if (!strcmp(cmd, "history")) list_all(OP_HISTORY, w[1] ? atoi(w[1]) : 0, 'h');
        else if (!strcmp(cmd, "myloans"))  list_all(OP_MY_LOANS, 0, 'l');
        else if (!strcmp(cmd, "assigned")) list_all(OP_ASSIGNED_LOANS, 0, 'l');
        else if (!strcmp(cmd, "pending"))  list_all(OP_PENDING_LOANS, 0, 'l');
        else if (!strcmp(cmd, "feedbacks")) list_all(OP_FEEDBACK_LIST, 0, 'f');
        else if (!strcmp(cmd, "addcustomer") && w[1] && w[2] && w[3] && w[4]) {
            ReqAddUser a; memset(&a, 0, sizeof a);
            copy_str(a.username, sizeof a.username, w[1]);
            copy_str(a.password, sizeof a.password, w[2]);
            copy_str(a.name, sizeof a.name, w[3]);
            a.initial = atoll(w[4]);
            if (call(OP_ADD_CUSTOMER, &a, sizeof a, &rep) == 0) { RespIds r; memcpy(&r, rep.payload, sizeof r); printf("  customer created: user %d account %d\n", r.id, r.id2); }
        }
        else if (!strcmp(cmd, "addstaff") && w[1] && w[2] && w[3] && w[4]) {
            ReqAddUser a; memset(&a, 0, sizeof a);
            copy_str(a.username, sizeof a.username, w[1]);
            copy_str(a.password, sizeof a.password, w[2]);
            copy_str(a.name, sizeof a.name, w[3]);
            a.role = role_from(w[4]);
            if (call(OP_ADD_STAFF, &a, sizeof a, &rep) == 0) { RespIds r; memcpy(&r, rep.payload, sizeof r); printf("  staff created: user %d\n", r.id); }
        }
        else if (!strcmp(cmd, "modify") && w[1] && w[2]) {
            ReqModify m; memset(&m, 0, sizeof m);
            m.user_id = atoi(w[1]);
            copy_str(m.name, sizeof m.name, w[2]);
            if (call(OP_MODIFY_USER, &m, sizeof m, &rep) == 0) puts("  ok");
        }
        else if (!strcmp(cmd, "process") && w[1] && w[2]) {
            ReqTwoInts t = { atoi(w[1]), atoi(w[2]) };
            if (call(OP_PROCESS_LOAN, &t, sizeof t, &rep) == 0) puts("  ok");
        }
        else if (!strcmp(cmd, "user") && w[1]) {
            ReqTwoInts t = { atoi(w[1]), 0 };
            if (call(OP_GET_USER, &t, sizeof t, &rep) == 0) print_user(&rep);
        }
        else if (!strcmp(cmd, "find") && w[1]) {
            ReqFind f; memset(&f, 0, sizeof f);
            copy_str(f.username, sizeof f.username, w[1]);
            if (call(OP_FIND_USER, &f, sizeof f, &rep) == 0) print_user(&rep);
        }
        else if ((!strcmp(cmd, "freeze") || !strcmp(cmd, "assign") || !strcmp(cmd, "setrole")) && w[1] && w[2]) {
            ReqTwoInts t = { atoi(w[1]), atoi(w[2]) };
            int op = !strcmp(cmd, "freeze") ? OP_SET_ACCOUNT_ACTIVE : !strcmp(cmd, "assign") ? OP_ASSIGN_LOAN : OP_SET_ROLE;
            if (op == OP_SET_ROLE) t.arg = role_from(w[2]);
            if (call(op, &t, sizeof t, &rep) == 0) puts("  ok");
        }
        else puts("unknown command or missing arguments (type 'help')");
        fflush(stdout);
    }
    cl_close(&conn);
    return 0;
}
