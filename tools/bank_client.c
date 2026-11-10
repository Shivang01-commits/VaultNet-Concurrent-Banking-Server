/*
 * bank_client - menu driven client.
 *
 *   ./bin/bank_client [host] [port]        (default 127.0.0.1 9090)
 *
 * It first asks you to log in.  After the login it shows a menu that contains
 * ONLY what your role may do (customer, employee, manager or administrator).
 * The server checks the rights again for every request, so this menu is
 * just a convenience.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "client.h"

static BankConn conn;
static RespLogin me;                 /* who is logged in (role, account) */
static char me_name[40];

/* ------------------------------------------------------------------ */
/* Reading input                                                       */
/* ------------------------------------------------------------------ */

static void bye(int code)
{
    cl_close(&conn);
    printf("\nGoodbye.\n");
    exit(code);
}

/* Show a prompt and read one line.  End of input (Ctrl-D) leaves the program. */
static void ask(const char *prompt, char *buf, size_t n)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) bye(0);
    if (!strchr(buf, '\n')) {                 /* input was longer than the buffer: throw away the rest of the line */
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) { }
    }
    buf[strcspn(buf, "\r\n")] = 0;
}

static void ask_password(const char *prompt, char *buf, size_t n)
{
    struct termios old, quiet;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    if (tty) { quiet = old; quiet.c_lflag &= ~ECHO; tcsetattr(0, TCSANOW, &quiet); }
    ask(prompt, buf, n);
    if (tty) { tcsetattr(0, TCSANOW, &old); printf("\n"); }
}

/* Read a whole number.  Returns 0 and fills *v, or -1 if it is not a number. */
static int ask_number(const char *prompt, long long *v)
{
    char line[64], *end;
    ask(prompt, line, sizeof line);
    if (line[0] == 0) return -1;
    *v = strtoll(line, &end, 10);
    if (*end != 0) { printf("  '%s' is not a number.\n", line); return -1; }
    return 0;
}

/* copy a string into a fixed field, cut if too long, always ending with '\0' */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* ------------------------------------------------------------------ */
/* Talking to the server                                               */
/* ------------------------------------------------------------------ */

/* One request.  Prints the reason when it failed.  Returns the status (0 = OK). */
static int call(int op, const void *p, size_t len, Packet *rep)
{
    Packet local;
    int st = cl_call(&conn, op, p, len, rep ? rep : &local);
    if (st == CL_E_IO) {
        printf("\nThe connection to the server was lost (it may have closed an idle connection).\n");
        bye(1);
    }
    if (st != 0) printf("  Not done: %s\n", proto_strerror(st));
    return st;
}

static const char *role_name(int r)
{
    return r == ROLE_CUSTOMER ? "Customer" : r == ROLE_EMPLOYEE ? "Employee" :
           r == ROLE_MANAGER ? "Manager" : r == ROLE_ADMIN ? "Administrator" : "?";
}
static const char *tx_name(int t)
{
    static const char *n[] = { "?", "Deposit", "Withdraw", "Transfer out", "Transfer in", "Loan credit" };
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
    strftime(buf, n, "%Y-%m-%d %H:%M", &tmv);
}

/* Look a user up by username (staff only).  Returns 0 and fills *u. */
static int find_user(const char *username, RespUser *u)
{
    ReqFind f;
    Packet rp;
    memset(&f, 0, sizeof f);
    copy_str(f.username, sizeof f.username, username);
    if (call(OP_FIND_USER, &f, sizeof f, &rp) != 0) return -1;
    PAYLOAD_GET(&rp, *u);
    return 0;
}

/* Ask for a username and look the user up. */
static int ask_user(const char *prompt, RespUser *u)
{
    char name[64];
    ask(prompt, name, sizeof name);
    if (name[0] == 0) return -1;
    return find_user(name, u);
}

/* Fetch and print every page of a list. kind: 'h' passbook, 'l' loans, 'f' feedback.
 * Returns how many items were printed (or -1 on error). */
static int list_all(int op, int id, char kind)
{
    ReqPage rq = { id, 0 };
    Packet rep;
    char tb[32];
    int total = 0;

    for (;;) {
        if (call(op, &rq, sizeof rq, &rep) != 0) return -1;
        int last = rq.after_id;
        for (int i = 0; i < rep.count; i++, total++) {
            if (kind == 'h') {
                TxnRec r; memcpy(&r, rep.payload + i * sizeof r, sizeof r);
                fmt_time(r.time, tb, sizeof tb);
                printf("  %s  %-12s %10lld   balance %lld", tb, tx_name(r.type), (long long)r.amount, (long long)r.balance_after);
                if (r.other_account) printf("   (account %d)", r.other_account);
                printf("\n");
                last = r.id;
            } else if (kind == 'l') {
                Loan l; memcpy(&l, rep.payload + i * sizeof l, sizeof l);
                printf("  Loan #%d   amount %lld   account %d   status %s", l.id, (long long)l.amount, l.account_id, loan_name(l.status));
                if (l.employee_id) printf("   employee %d", l.employee_id);
                printf("\n");
                last = l.id;
            } else {
                Feedback f; memcpy(&f, rep.payload + i * sizeof f, sizeof f);
                fmt_time(f.time, tb, sizeof tb);
                printf("  [%s] %s: %.*s\n", tb, f.username, TEXT_LEN, f.text);
                last = f.id;
            }
        }
        if (!(rep.flags & FLAG_MORE)) break;
        rq.after_id = last;
    }
    if (total == 0) printf("  (nothing to show)\n");
    return total;
}

/* ------------------------------------------------------------------ */
/* Menu actions (each one is a small function)                         */
/* ------------------------------------------------------------------ */

/* ---- every role ---- */
static void act_password(void)
{
    ReqText t;
    char a[64], b[64];
    memset(&t, 0, sizeof t);
    ask_password("  New password: ", a, sizeof a);
    ask_password("  Repeat it:    ", b, sizeof b);
    if (a[0] == 0 || strcmp(a, b) != 0) { printf("  The two passwords are different (or empty). Nothing changed.\n"); return; }
    copy_str(t.text, sizeof t.text, a);
    if (call(OP_PASSWD, &t, sizeof t, NULL) == 0) printf("  Password changed.\n");
}

/* ---- customer ---- */
static void act_balance(void)
{
    Packet rp; RespBalance b;
    if (call(OP_BALANCE, NULL, 0, &rp) == 0) { PAYLOAD_GET(&rp, b); printf("  Your balance: %lld\n", (long long)b.balance); }
}

static void money_op(int op, const char *what)
{
    long long amount;
    Packet rp; RespBalance b;
    char prompt[64];
    snprintf(prompt, sizeof prompt, "  Amount to %s: ", what);
    if (ask_number(prompt, &amount) != 0) return;
    ReqAmount a = { amount };
    if (call(op, &a, sizeof a, &rp) == 0) { PAYLOAD_GET(&rp, b); printf("  Done. New balance: %lld\n", (long long)b.balance); }
}
static void act_deposit(void)  { money_op(OP_DEPOSIT, "deposit"); }
static void act_withdraw(void) { money_op(OP_WITHDRAW, "withdraw"); }

static void act_transfer(void)
{
    long long to, amount;
    Packet rp; RespBalance b;
    if (ask_number("  Receiver's account number: ", &to) != 0) return;
    if (ask_number("  Amount to transfer: ", &amount) != 0) return;
    ReqTransfer t; memset(&t, 0, sizeof t);
    t.to_account = (int)to; t.amount = amount;
    if (call(OP_TRANSFER, &t, sizeof t, &rp) == 0) { PAYLOAD_GET(&rp, b); printf("  Done. New balance: %lld\n", (long long)b.balance); }
}

static void act_apply_loan(void)
{
    long long amount;
    Packet rp; RespIds r;
    if (ask_number("  Loan amount: ", &amount) != 0) return;
    ReqAmount a = { amount };
    if (call(OP_LOAN_APPLY, &a, sizeof a, &rp) == 0) { PAYLOAD_GET(&rp, r); printf("  Application sent. Your loan number is %d.\n", r.id); }
}

static void act_my_loans(void) { list_all(OP_MY_LOANS, 0, 'l'); }

static void act_feedback(void)
{
    ReqText t; memset(&t, 0, sizeof t);
    char text[TEXT_LEN];
    ask("  Your feedback (max 127 characters): ", text, sizeof text);
    if (text[0] == 0) return;
    copy_str(t.text, sizeof t.text, text);
    if (call(OP_FEEDBACK_ADD, &t, sizeof t, NULL) == 0) printf("  Thank you, feedback saved.\n");
}

static void act_my_history(void) { list_all(OP_HISTORY, 0, 'h'); }

/* ---- employee ---- */
static void act_add_customer(void)
{
    ReqAddUser a; Packet rp; RespIds r;
    char s[64]; long long initial;
    memset(&a, 0, sizeof a);
    ask("  Username for the new customer: ", s, sizeof s);  if (!s[0]) return;  copy_str(a.username, sizeof a.username, s);
    ask_password("  Password: ", s, sizeof s);              if (!s[0]) return;  copy_str(a.password, sizeof a.password, s);
    ask("  Full name: ", s, sizeof s);                      if (!s[0]) return;  copy_str(a.name, sizeof a.name, s);
    if (ask_number("  Opening deposit: ", &initial) != 0) return;
    a.initial = initial;
    if (call(OP_ADD_CUSTOMER, &a, sizeof a, &rp) == 0) {
        PAYLOAD_GET(&rp, r);
        printf("  Customer created. User id %d, ACCOUNT NUMBER %d.\n", r.id, r.id2);
    }
}

static void act_modify_user(void)
{
    RespUser u; ReqModify m; char name[64];
    if (ask_user("  Username of the person to modify: ", &u) != 0) return;
    printf("  Found: %s, %s, role %s.\n", u.username, u.name, role_name(u.role));
    ask("  New full name: ", name, sizeof name);
    if (!name[0]) return;
    memset(&m, 0, sizeof m);
    m.user_id = u.id;
    copy_str(m.name, sizeof m.name, name);
    if (call(OP_MODIFY_USER, &m, sizeof m, NULL) == 0) printf("  Details updated.\n");
}

static void act_assigned_loans(void) { list_all(OP_ASSIGNED_LOANS, 0, 'l'); }

static void act_process_loan(void)
{
    long long id; char c[16];
    printf("  Loans waiting for your decision:\n");
    if (list_all(OP_ASSIGNED_LOANS, 0, 'l') <= 0) return;
    if (ask_number("  Loan number to decide: ", &id) != 0) return;
    ask("  Approve or reject? (a/r): ", c, sizeof c);
    if (c[0] != 'a' && c[0] != 'A' && c[0] != 'r' && c[0] != 'R') { printf("  Cancelled.\n"); return; }
    ReqTwoInts t = { (int)id, (c[0] == 'a' || c[0] == 'A') ? 1 : 0 };
    if (call(OP_PROCESS_LOAN, &t, sizeof t, NULL) == 0)
        printf("  Loan %lld %s.\n", id, t.arg ? "approved and the money was credited" : "rejected");
}

static void act_customer_passbook(void)
{
    RespUser u;
    if (ask_user("  Customer's username: ", &u) != 0) return;
    if (u.account_id == 0) { printf("  %s has no bank account.\n", u.username); return; }
    printf("  Passbook of %s (account %d):\n", u.name, u.account_id);
    list_all(OP_HISTORY, u.account_id, 'h');
}

/* ---- manager ---- */
static void act_set_active(void)
{
    RespUser u; char c[16];
    if (ask_user("  Customer's username: ", &u) != 0) return;
    if (u.account_id == 0) { printf("  %s has no bank account.\n", u.username); return; }
    printf("  Account %d of %s is currently %s.\n", u.account_id, u.name, u.active ? "user active" : "user inactive");
    ask("  Activate or deactivate the account? (a/d): ", c, sizeof c);
    if (c[0] != 'a' && c[0] != 'A' && c[0] != 'd' && c[0] != 'D') { printf("  Cancelled.\n"); return; }
    ReqTwoInts t = { u.account_id, (c[0] == 'a' || c[0] == 'A') ? 1 : 0 };
    if (call(OP_SET_ACCOUNT_ACTIVE, &t, sizeof t, NULL) == 0)
        printf("  Account %d is now %s.\n", u.account_id, t.arg ? "ACTIVE" : "DEACTIVATED");
}

static void act_assign_loan(void)
{
    long long id; RespUser emp;
    printf("  Loans waiting to be assigned:\n");
    if (list_all(OP_PENDING_LOANS, 0, 'l') <= 0) return;
    if (ask_number("  Loan number: ", &id) != 0) return;
    if (ask_user("  Username of the employee who will process it: ", &emp) != 0) return;
    ReqTwoInts t = { (int)id, emp.id };
    if (call(OP_ASSIGN_LOAN, &t, sizeof t, NULL) == 0) printf("  Loan %lld assigned to %s.\n", id, emp.name);
}

static void act_feedback_list(void) { list_all(OP_FEEDBACK_LIST, 0, 'f'); }

/* ---- administrator ---- */
static void act_add_staff(void)
{
    ReqAddUser a; Packet rp; RespIds r;
    char s[64]; long long role;
    memset(&a, 0, sizeof a);
    ask("  Username for the new staff member: ", s, sizeof s);  if (!s[0]) return;  copy_str(a.username, sizeof a.username, s);
    ask_password("  Password: ", s, sizeof s);                   if (!s[0]) return;  copy_str(a.password, sizeof a.password, s);
    ask("  Full name: ", s, sizeof s);                           if (!s[0]) return;  copy_str(a.name, sizeof a.name, s);
    if (ask_number("  Role (1 = employee, 2 = manager): ", &role) != 0 || (role != 1 && role != 2)) { printf("  Cancelled.\n"); return; }
    a.role = role == 1 ? ROLE_EMPLOYEE : ROLE_MANAGER;
    if (call(OP_ADD_STAFF, &a, sizeof a, &rp) == 0) { PAYLOAD_GET(&rp, r); printf("  Staff member created. User id %d.\n", r.id); }
}

static void act_set_role(void)
{
    RespUser u; long long role;
    if (ask_user("  Username of the staff member: ", &u) != 0) return;
    printf("  %s is currently: %s.\n", u.name, role_name(u.role));
    if (ask_number("  New role (1 = employee, 2 = manager): ", &role) != 0 || (role != 1 && role != 2)) { printf("  Cancelled.\n"); return; }
    ReqTwoInts t = { u.id, role == 1 ? ROLE_EMPLOYEE : ROLE_MANAGER };
    if (call(OP_SET_ROLE, &t, sizeof t, NULL) == 0) printf("  %s is now %s.\n", u.name, role_name(t.arg));
}

/* ------------------------------------------------------------------ */
/* Menus: one table per role                                           */
/* ------------------------------------------------------------------ */

typedef struct { const char *label; void (*run)(void); } MenuItem;

static const MenuItem CUSTOMER_MENU[] = {
    { "View account balance",        act_balance },
    { "Deposit money",               act_deposit },
    { "Withdraw money",              act_withdraw },
    { "Transfer funds",              act_transfer },
    { "Apply for a loan",            act_apply_loan },
    { "View my loan applications",   act_my_loans },
    { "Change password",             act_password },
    { "Add feedback",                act_feedback },
    { "View transaction history",    act_my_history },
};
static const MenuItem EMPLOYEE_MENU[] = {
    { "Add new customer",                      act_add_customer },
    { "Modify customer details",               act_modify_user },
    { "View assigned loan applications",       act_assigned_loans },
    { "Process loan (approve / reject)",       act_process_loan },
    { "View customer transactions (passbook)", act_customer_passbook },
    { "Change password",                       act_password },
};
static const MenuItem MANAGER_MENU[] = {
    { "Activate / deactivate customer account", act_set_active },
    { "Assign loan application to an employee", act_assign_loan },
    { "Review customer feedback",               act_feedback_list },
    { "Change password",                        act_password },
};
static const MenuItem ADMIN_MENU[] = {
    { "Add new bank employee / manager",   act_add_staff },
    { "Modify customer / employee details", act_modify_user },
    { "Manage user roles",                 act_set_role },
    { "Change password",                   act_password },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Log in.  Returns 1 if logged in. */
static int do_login(void)
{
    char user[64], pass[64];
    ask("  Username: ", user, sizeof user);
    ask_password("  Password: ", pass, sizeof pass);
    if (!user[0]) return 0;
    int st = cl_login(&conn, user, pass, &me);
    if (st == CL_E_IO) { printf("  The connection to the server was lost.\n"); bye(1); }
    if (st != 0) { printf("  Login failed: %s\n", proto_strerror(st)); return 0; }
    copy_str(me_name, sizeof me_name, user);
    return 1;
}

/* Show the menu of the logged-in role and run the chosen action.
 * Returns 0 when the user logged out. */
static int role_menu(void)
{
    const MenuItem *items;
    int n;
    switch (me.role) {
    case ROLE_CUSTOMER: items = CUSTOMER_MENU; n = COUNT(CUSTOMER_MENU); break;
    case ROLE_EMPLOYEE: items = EMPLOYEE_MENU; n = COUNT(EMPLOYEE_MENU); break;
    case ROLE_MANAGER:  items = MANAGER_MENU;  n = COUNT(MANAGER_MENU);  break;
    default:            items = ADMIN_MENU;    n = COUNT(ADMIN_MENU);    break;
    }

    printf("\n===== %s menu - logged in as %s", role_name(me.role), me_name);
    if (me.role == ROLE_CUSTOMER) printf(" (account %d)", me.account_id);
    printf(" =====\n");
    for (int i = 0; i < n; i++) printf("  %2d. %s\n", i + 1, items[i].label);
    printf("  %2d. Logout\n  %2d. Exit\n", n + 1, n + 2);

    long long c;
    if (ask_number("Choose an option: ", &c) != 0) return 1;
    printf("\n");
    if (c >= 1 && c <= n) { items[c - 1].run(); return 1; }
    if (c == n + 1) { call(OP_LOGOUT, NULL, 0, NULL); printf("  Logged out.\n"); return 0; }
    if (c == n + 2) { call(OP_LOGOUT, NULL, 0, NULL); bye(0); }
    printf("  Please choose a number from the menu.\n");
    return 1;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 9090;

    if (cl_connect(&conn, host, port) < 0) {
        fprintf(stderr, "Cannot connect to the bank server at %s:%d. Is it running?\n", host, port);
        return 1;
    }
    printf("========================================\n   BANKING MANAGEMENT SYSTEM\n========================================\n");

    for (;;) {
        printf("\n  1. Login\n  2. Exit\n");
        long long c;
        if (ask_number("Choose an option: ", &c) != 0) continue;
        if (c == 2) bye(0);
        if (c != 1) { printf("  Please choose 1 or 2.\n"); continue; }
        if (!do_login()) continue;
        while (role_menu()) {}
    }
}
