/*
 * test_atomicity.c - "A" of ACID: a transaction happens COMPLETELY or NOT AT ALL.
 *
 * We make a child process do a transfer of 100 from customer c0 to c1 and
 * make it kill itself (like `kill -9`) at each of the 4 dangerous moments
 * inside txn_commit() (see journal.h).  Then we check what the bank looks like.
 *
 *   crash point 1: journal written, NOT committed  -> transfer must vanish
 *   crash point 2: committed, nothing copied yet   -> transfer must be completed
 *   crash point 3: committed, ONE record copied    -> torn data on disk, must be repaired
 *   crash point 4: everything copied, slot not cleared -> must not be applied twice
 */
#include "test_helpers.h"
#include "bank_internal.h"

/* One experiment.
 *   point        which crash point the child dies at
 *   via_startup  0 = another worker repairs lazily while doing a normal request
 *                1 = repair happens when a new Db handle is opened (startup)
 *   applied      do we expect the transfer to be in effect afterwards? */
static void crash_case(int point, int via_startup, int applied)
{
    char dir[64], title[120];
    Db db;
    TestBank tb;

    snprintf(title, sizeof title, "crash at point %d, repaired %s", point,
             via_startup ? "at startup (db_open)" : "lazily by the next request");
    SCENARIO(title);

    make_test_dir(dir);
    setup_bank(&db, dir, 2, 1000, &tb);

    /* ---- child: do the transfer and die in the middle ---- */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Db child_db;
        int64_t nb;
        if (db_open(&child_db, dir) < 0) _exit(2);   /* each process opens its OWN Db */
        journal_crash_point = point;                 /* die at this step of txn_commit */
        bank_transfer(&child_db, &tb.cust[0], tb.acct[1], 100, &nb);
        _exit(0);                                    /* only reached if it did not crash */
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(died_by_sigkill(status), "the worker really died in the middle of the transfer");

    /* ---- look at the raw files BEFORE any repair ---- */
    int64_t raw0 = raw_balance(&db, tb.acct[0]);
    int64_t raw1 = raw_balance(&db, tb.acct[1]);
    printf("      raw files right after the crash: c0=%lld c1=%lld (total %lld)\n",
           (long long)raw0, (long long)raw1, (long long)(raw0 + raw1));
    if (point == 3)
        CHECK(raw0 + raw1 != 2000, "data files really were torn (money missing) before repair");

    /* ---- repair ---- */
    Db db2;
    if (via_startup) {
        if (db_open(&db2, dir) < 0) die("db_open 2");  /* startup recovery runs inside */
    } else {
        int64_t b;
        /* An ordinary balance request: it locks c0's record, notices the dead
         * worker's COMMITTED journal slot touching that record, and finishes it. */
        bank_balance(&db, &tb.cust[0], &b);
        db2 = db;
    }

    int64_t b0 = raw_balance(&db2, tb.acct[0]);
    int64_t b1 = raw_balance(&db2, tb.acct[1]);
    printf("      after repair:                    c0=%lld c1=%lld\n", (long long)b0, (long long)b1);

    if (applied) {
        CHECK(b0 == 900 && b1 == 1100, "transfer is completely applied (900 / 1100)");
        CHECK(history_count(&db, &tb, tb.acct[0]) == 2, "sender passbook has the transfer line");
        CHECK(history_count(&db, &tb, tb.acct[1]) == 2, "receiver passbook has the transfer line");
    } else {
        CHECK(b0 == 1000 && b1 == 1000, "transfer vanished completely (1000 / 1000)");
        CHECK(history_count(&db, &tb, tb.acct[0]) == 1, "sender passbook has NO transfer line");
    }
    CHECK(total_money(&db2) == 2000, "total money in the bank is unchanged (2000)");
    CHECK(history_matches_balance(&db, &tb), "passbooks add up to the balances");

    remove_test_dir(dir);
}

/* A rule fails (not enough money): nothing may change and locks must be released. */
static void rule_failure_case(void)
{
    char dir[64];
    Db db, other;
    TestBank tb;
    int64_t nb;

    SCENARIO("transfer rejected by a rule (insufficient funds)");
    make_test_dir(dir);
    setup_bank(&db, dir, 2, 1000, &tb);
    if (db_open(&other, dir) < 0) die("db_open other");

    int rc = bank_transfer(&db, &tb.cust[0], tb.acct[1], 5000, &nb);
    CHECK(rc == BANK_E_INSUFFICIENT, "transfer of 5000 from a 1000 account is refused");
    CHECK(raw_balance(&db, tb.acct[0]) == 1000 && raw_balance(&db, tb.acct[1]) == 1000,
          "both balances unchanged");
    CHECK(history_count(&db, &tb, tb.acct[0]) == 1, "no passbook line was written");

    /* The failed request must have released its locks: another worker can lock at once. */
    CHECK(db_lock_record(&other, F_ACCOUNTS, tb.acct[0], LK_WRITE, 0) == 0,
          "record lock was released after the failure");
    db_unlock_record(&other, F_ACCOUNTS, tb.acct[0]);

    CHECK(bank_withdraw(&db, &tb.cust[0], -5, &nb) == BANK_E_INVALID, "negative amount is refused");
    CHECK(bank_transfer(&db, &tb.cust[0], tb.acct[0], 5, &nb) == BANK_E_INVALID, "transfer to self is refused");
    CHECK(bank_transfer(&db, &tb.cust[0], 9999, 5, &nb) == BANK_E_NOT_FOUND, "transfer to missing account is refused");
    CHECK(total_money(&db) == 2000, "total money unchanged");
    remove_test_dir(dir);
}

/* Creating a user touches users.dat, names.dat (index), accounts.dat and txns.dat in
 * ONE transaction.  Crash at each point: the user must exist completely (findable by
 * name, account with its first passbook line) or not at all - never half. */
static void create_user_crash_case(int point, int applied)
{
    char dir[64], title[100];
    Db db, db2;
    TestBank tb;
    User u;
    int uid;

    snprintf(title, sizeof title, "create user, crash at point %d (name index + user + account + first line)", point);
    SCENARIO(title);
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 1000, &tb);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Db c;
        if (db_open(&c, dir) < 0) _exit(2);
        journal_crash_point = point;
        create_user(&c, "newbie", "pw", "New Bie", ROLE_CUSTOMER, 500, &uid);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(died_by_sigkill(status), "the creating process really died");

    if (db_open(&db2, dir) < 0) die("db_open 2");       /* startup recovery */
    int found = user_find_by_name(&db2, "newbie", &u) == BANK_OK;
    if (applied) {
        CHECK(found && u.role == ROLE_CUSTOMER, "user is findable through the index");
        CHECK(found && raw_balance(&db2, u.account_id) == 500, "account exists with the opening deposit");
        CHECK(found && history_count(&db2, &tb, u.account_id) == 1, "passbook chain has the opening line");
        CHECK(create_user(&db2, "newbie", "pw", "Again", ROLE_CUSTOMER, 0, &uid) == BANK_E_EXISTS,
              "creating the same name again is refused");
    } else {
        CHECK(!found, "user does not exist (not in the index either)");
        CHECK(create_user(&db2, "newbie", "pw", "New Bie", ROLE_CUSTOMER, 500, &uid) == BANK_OK,
              "the name is free again: creating it now works");
        CHECK(user_find_by_name(&db2, "newbie", &u) == BANK_OK, "and it is found");
    }
    remove_test_dir(dir);
}

int main(void)
{
    printf("ATOMICITY TESTS (all-or-nothing)\n");

    crash_case(1, 0, 0);     /* not committed -> must vanish          */
    crash_case(2, 0, 1);     /* committed     -> must be completed    */
    crash_case(3, 0, 1);     /* half applied  -> must be completed    */
    crash_case(4, 0, 1);     /* fully applied -> must not run twice   */
    crash_case(1, 1, 0);     /* same four, but repaired at startup    */
    crash_case(2, 1, 1);
    crash_case(3, 1, 1);
    crash_case(4, 1, 1);
    rule_failure_case();
    create_user_crash_case(1, 0);
    create_user_crash_case(2, 1);
    create_user_crash_case(3, 1);
    create_user_crash_case(4, 1);

    return test_finish("test_atomicity");
}
