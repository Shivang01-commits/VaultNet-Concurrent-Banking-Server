/*
 * test_consistency.c - "C" of ACID: the bank's rules always hold.
 *
 * The rules we check after heavy, messy activity:
 *   1. money is never created or destroyed by transfers (total is constant)
 *   2. no balance is ever negative
 *   3. every account's passbook adds up exactly to its balance
 *   4. a loan can be approved only once
 *   5. a frozen (deactivated) account cannot be used
 */
#include "test_helpers.h"

#define CUSTOMERS 6
#define START     1000

/* Each child does `ops` random transfers between the customers.
 * If chaos != 0, about 1 in 40 requests kills the process in the middle of
 * the commit (random crash point) - a worker crashing under load. */
static void random_worker(const char *dir, TestBank *tb, int ops, int chaos)
{
    Db db;
    int64_t nb;

    if (db_open(&db, dir) < 0) _exit(2);
    srand(getpid());
    for (int i = 0; i < ops; i++) {
        int from = rand() % tb->n;
        int to   = rand() % tb->n;
        if (from == to) continue;
        if (chaos && rand() % 40 == 0) journal_crash_point = 1 + rand() % 4;
        bank_transfer(&db, &tb->cust[from], tb->acct[to], 1 + rand() % 150, &nb);
        journal_crash_point = 0;
        if (rand() % 5 == 0) bank_deposit(&db, &tb->cust[from], 1 + rand() % 20, &nb);
        if (rand() % 5 == 0) bank_withdraw(&db, &tb->cust[from], 1 + rand() % 20, &nb);
    }
    _exit(0);
}

/* the rules, checked on a fresh Db handle (which also runs startup recovery) */
static void check_rules(const char *dir, TestBank *tb, int64_t expected_total_min_change)
{
    Db db;
    (void)expected_total_min_change;
    if (db_open(&db, dir) < 0) die("db_open");

    int negative = 0;
    for (int i = 0; i < tb->n; i++)
        if (raw_balance(&db, tb->acct[i]) < 0) negative = 1;
    CHECK(!negative, "no account has a negative balance");
    CHECK(history_matches_balance(&db, tb), "every passbook adds up exactly to its balance");
    db_close(&db);
}

/* Random transfers from several processes. Deposits/withdrawals change the
 * total, so here we track money with transfers only: use a separate scenario. */
static void transfers_only(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    int64_t nb;

    SCENARIO("6 processes x 300 random transfers (money must be conserved)");
    make_test_dir(dir);
    db_sync_enabled = 0;                       /* faster; kill -9 cannot lose page-cache data */
    setup_bank(&db, dir, CUSTOMERS, START, &tb);

    fflush(stdout);
    for (int p = 0; p < 6; p++) {
        if (fork() == 0) {
            Db c;
            if (db_open(&c, dir) < 0) _exit(2);
            srand(getpid());
            for (int i = 0; i < 300; i++) {
                int from = rand() % CUSTOMERS, to = rand() % CUSTOMERS;
                if (from != to)
                    bank_transfer(&c, &tb.cust[from], tb.acct[to], 1 + rand() % 200, &nb);
            }
            _exit(0);
        }
    }
    int status, bad = 0;
    while (wait(&status) > 0) if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) bad = 1;
    CHECK(!bad, "all worker processes finished normally");
    CHECK(total_money(&db) == (int64_t)CUSTOMERS * START, "total money is exactly what we started with");
    check_rules(dir, &tb, 0);
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

/* Same, but workers keep DYING in the middle of commits and are replaced. */
static void chaos(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("workers keep crashing mid-commit (kill -9) while others keep working");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, CUSTOMERS, START, &tb);

    /* Only transfers here (no deposits/withdrawals), so total must stay constant.
     * We reuse random_worker but it also deposits/withdraws; so build a tiny
     * transfer-only chaos worker inline. */
    int spawned = 0, crashed = 0, running = 0, status;
    fflush(stdout);
    while (spawned < 40 || running > 0) {
        while (running < 4 && spawned < 40) {
            if (fork() == 0) {
                Db c;
                int64_t nb;
                if (db_open(&c, dir) < 0) _exit(2);
                srand(getpid());
                for (int i = 0; i < 150; i++) {
                    int from = rand() % CUSTOMERS, to = rand() % CUSTOMERS;
                    if (from == to) continue;
                    if (rand() % 40 == 0) journal_crash_point = 1 + rand() % 4;  /* die mid-commit */
                    bank_transfer(&c, &tb.cust[from], tb.acct[to], 1 + rand() % 150, &nb);
                    journal_crash_point = 0;
                }
                _exit(0);
            }
            spawned++; running++;
        }
        if (wait(&status) > 0) {
            running--;
            if (died_by_sigkill(status)) crashed++;
        }
    }
    printf("      %d workers started, %d of them were killed in the middle of a commit\n", spawned, crashed);
    CHECK(crashed > 0, "the test really did crash some workers");

    /* Startup recovery finishes whatever the last dead workers left behind. */
    Db fresh;
    if (db_open(&fresh, dir) < 0) die("db_open fresh");
    CHECK(total_money(&fresh) == (int64_t)CUSTOMERS * START,
          "total money is still exactly what we started with");
    db_close(&fresh);
    check_rules(dir, &tb, 0);
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

/* A loan can only be approved once, even if two employees' requests race. */
static void loan_rules(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    int loan_id, rc;

    SCENARIO("loan workflow and 'approve only once' rule");
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 1000, &tb);

    CHECK(bank_apply_loan(&db, &tb.cust[0], 5000, &loan_id) == BANK_OK, "customer applies for a loan");
    rc = bank_process_loan(&db, &tb.emp, loan_id, 1);
    CHECK(rc == BANK_E_PERMISSION, "employee cannot process a loan that is not assigned to them");
    CHECK(bank_assign_loan(&db, &tb.emp, loan_id, tb.emp.id) == BANK_E_PERMISSION, "employee cannot assign loans (manager only)");
    CHECK(bank_assign_loan(&db, &tb.mgr, loan_id, tb.emp.id) == BANK_OK, "manager assigns the loan to the employee");

    /* two processes try to approve at the same moment */
    fflush(stdout);
    for (int p = 0; p < 2; p++) {
        if (fork() == 0) {
            Db c;
            if (db_open(&c, dir) < 0) _exit(2);
            _exit(bank_process_loan(&c, &tb.emp, loan_id, 1) == BANK_OK ? 0 : 1);
        }
    }
    int status, ok = 0;
    while (wait(&status) > 0) if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ok++;
    CHECK(ok == 1, "exactly one of two racing approvals succeeded");
    CHECK(raw_balance(&db, tb.acct[0]) == 6000, "the loan amount was credited exactly once (1000 + 5000)");
    CHECK(history_matches_balance(&db, &tb), "passbook shows the loan credit");
    remove_test_dir(dir);
}

/* Deactivated accounts cannot move money. */
static void frozen_account(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    int64_t nb;

    SCENARIO("deactivated (frozen) account");
    make_test_dir(dir);
    setup_bank(&db, dir, 2, 1000, &tb);

    CHECK(bank_set_account_active(&db, &tb.emp, tb.acct[0], 0) == BANK_E_PERMISSION, "only a manager can freeze an account");
    CHECK(bank_set_account_active(&db, &tb.mgr, tb.acct[0], 0) == BANK_OK, "manager freezes c0's account");
    CHECK(bank_deposit(&db, &tb.cust[0], 10, &nb)  == BANK_E_INACTIVE, "deposit to frozen account refused");
    CHECK(bank_withdraw(&db, &tb.cust[0], 10, &nb) == BANK_E_INACTIVE, "withdraw from frozen account refused");
    CHECK(bank_transfer(&db, &tb.cust[1], tb.acct[0], 10, &nb) == BANK_E_INACTIVE, "transfer INTO frozen account refused");
    CHECK(bank_set_account_active(&db, &tb.mgr, tb.acct[0], 1) == BANK_OK, "manager unfreezes it");
    CHECK(bank_deposit(&db, &tb.cust[0], 10, &nb) == BANK_OK, "deposit works again");
    remove_test_dir(dir);
}

int main(void)
{
    printf("CONSISTENCY TESTS (rules always hold)\n");
    transfers_only();
    chaos();
    loan_rules();
    frozen_account();
    (void)random_worker;
    return test_finish("test_consistency");
}
