/*
 * test_helpers.h - tiny helpers shared by all tests (no test framework needed).
 *
 *   CHECK(condition, "what we expect")   prints ok / FAIL and counts failures
 *   setup_bank(...)                      creates a fresh bank with N customers
 *   total_money(...)                     sums every account balance
 *   history_matches_balance(...)         passbook lines add up to the balance
 */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bank.h"
#include "journal.h"

static int g_checks = 0, g_failures = 0;

#define CHECK(cond, msg) do {                                              \
        g_checks++;                                                        \
        if (cond) { printf("    ok    %s\n", msg); }                       \
        else { g_failures++; printf("    FAIL  %s   (line %d)\n", msg, __LINE__); } \
    } while (0)

/* print a heading for one scenario */
#define SCENARIO(title) do { printf("\n  * %s\n", title); fflush(stdout); } while (0)

/* Print the final verdict of a test program and give its exit code. */
static inline int test_finish(const char *name)
{
    printf("\n%s: %d checks, %d failed -> %s\n", name, g_checks, g_failures,
           g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}

static inline void die(const char *msg)
{
    fprintf(stderr, "test setup problem: %s\n", msg);
    exit(2);
}

/* Make a fresh empty directory under /tmp for one scenario. */
static inline void make_test_dir(char *dir /* at least 40 bytes */)
{
    strcpy(dir, "/tmp/bank_test_XXXXXX");
    if (!mkdtemp(dir)) die("mkdtemp");
}

static inline void remove_test_dir(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* ignore */ }
}

/* ------------------------------------------------------------------ */
/* A ready-made bank: admin, one employee, one manager, N customers    */
/* ------------------------------------------------------------------ */
#define MAX_CUST 16

typedef struct {
    User admin, emp, mgr;
    User cust[MAX_CUST];     /* logged-in User structs (password for all = "pw") */
    int  acct[MAX_CUST];     /* their account ids                                */
    int  n;
} TestBank;

/* Opens the Db in `dir`, creates users named c0, c1, ... each with `initial` money.
 * The Db stays open for the caller. */
static inline void setup_bank(Db *db, const char *dir, int n, int64_t initial, TestBank *tb)
{
    int id;
    char name[32];

    memset(tb, 0, sizeof *tb);
    if (n > MAX_CUST) die("too many customers");
    if (db_open(db, dir) < 0) die("db_open");
    if (bank_create_first_admin(db, "admin", "pw") != BANK_OK) die("first admin");
    if (bank_authenticate(db, "admin", "pw", &tb->admin) != BANK_OK) die("login admin");

    if (bank_add_employee(db, &tb->admin, "emp", "pw", "Employee", ROLE_EMPLOYEE, &id) != BANK_OK) die("add emp");
    if (bank_add_employee(db, &tb->admin, "mgr", "pw", "Manager", ROLE_MANAGER, &id) != BANK_OK) die("add mgr");
    if (bank_authenticate(db, "emp", "pw", &tb->emp) != BANK_OK) die("login emp");
    if (bank_authenticate(db, "mgr", "pw", &tb->mgr) != BANK_OK) die("login mgr");

    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof name, "c%d", i);
        if (bank_add_customer(db, &tb->emp, name, "pw", name, initial, &id) != BANK_OK) die("add customer");
        if (bank_authenticate(db, name, "pw", &tb->cust[i]) != BANK_OK) die("login customer");
        tb->acct[i] = tb->cust[i].account_id;
    }
    tb->n = n;
}

/* Sum of all balances, read straight from accounts.dat. */
static inline int64_t total_money(Db *db)
{
    Account a;
    int64_t sum = 0;
    int n = db_count(db, F_ACCOUNTS);
    for (int i = 1; i < n; i++) {
        db_read(db, F_ACCOUNTS, i, &a);
        if (a.id != 0) sum += a.balance;
    }
    return sum;
}

/* Read one account's balance straight from the file (no locks, no recovery). */
static inline int64_t raw_balance(Db *db, int account_id)
{
    Account a;
    db_read(db, F_ACCOUNTS, account_id, &a);
    return a.balance;
}

/* Passbook check: money in minus money out over all passbook lines must be
 * exactly the balance.  Returns 1 if it holds for every customer. */
static inline int history_matches_balance(Db *db, TestBank *tb)
{
    int ok = 1;
    int cap = 200000;
    TxnRec *lines = malloc(sizeof(TxnRec) * cap);
    if (!lines) die("malloc");

    for (int i = 0; i < tb->n; i++) {
        int count = 0;
        if (bank_history(db, &tb->emp, tb->acct[i], lines, cap, &count) != BANK_OK) { ok = 0; continue; }
        int64_t sum = 0;
        for (int k = 0; k < count; k++) {
            int t = lines[k].type;
            if (t == TX_DEPOSIT || t == TX_TRANSFER_IN || t == TX_LOAN_CREDIT) sum += lines[k].amount;
            else sum -= lines[k].amount;
        }
        if (sum != raw_balance(db, tb->acct[i])) {
            printf("      passbook of account %d adds to %lld but balance is %lld\n", tb->acct[i],
                   (long long)sum, (long long)raw_balance(db, tb->acct[i]));
            ok = 0;
        }
    }
    free(lines);
    return ok;
}

/* How many passbook lines does this account have? */
static inline int history_count(Db *db, TestBank *tb, int account_id)
{
    static TxnRec lines[5000];
    int count = 0;
    if (bank_history(db, &tb->emp, account_id, lines, 5000, &count) != BANK_OK) return -1;
    return count;
}

/* Did this child die from kill -9 ? */
static inline int died_by_sigkill(int status)
{
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

#endif /* TEST_HELPERS_H */
