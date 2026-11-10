/*
 * test_durability.c - "D" of ACID: once the bank says "OK", the data survives.
 *
 *   A. process is killed right after a successful request -> data still there
 *   B. data is really in the FILES (a brand new Db sees it)
 *   C. committed-but-not-yet-copied work is not lost (journal replay)
 *   D. id counters never go backwards (no id reused after restart / stale counters)
 *
 * (A real power-cut test needs a VM; kill -9 is the closest we can do in a test.
 *  The code calls fsync() at the commit point so that data also survives power loss.)
 */
#include "test_helpers.h"

static void survives_kill(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("process is killed (kill -9) right after a successful deposit");
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 1000, &tb);

    fflush(stdout);
    if (fork() == 0) {
        Db c;
        int64_t nb;
        if (db_open(&c, dir) < 0) _exit(2);
        if (bank_deposit(&c, &tb.cust[0], 250, &nb) != BANK_OK) _exit(3);
        kill(getpid(), SIGKILL);                 /* die immediately after "OK" */
        _exit(0);
    }
    int status;
    wait(&status);
    CHECK(died_by_sigkill(status), "the process was killed after its deposit returned OK");

    Db fresh;                                    /* a completely new handle, reading the files */
    if (db_open(&fresh, dir) < 0) die("db_open");
    CHECK(raw_balance(&fresh, tb.acct[0]) == 1250, "the deposit is in the file (1000 + 250)");
    CHECK(history_count(&fresh, &tb, tb.acct[0]) == 2, "its passbook line is in the file too");
    remove_test_dir(dir);
}

static void committed_work_not_lost(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("crash AFTER the commit point but BEFORE data files were updated");
    make_test_dir(dir);
    setup_bank(&db, dir, 2, 1000, &tb);

    fflush(stdout);
    if (fork() == 0) {
        Db c;
        int64_t nb;
        if (db_open(&c, dir) < 0) _exit(2);
        journal_crash_point = 2;                 /* committed, nothing copied */
        bank_transfer(&c, &tb.cust[0], tb.acct[1], 400, &nb);
        _exit(0);
    }
    int status;
    wait(&status);
    CHECK(died_by_sigkill(status), "worker died right after the commit point");
    CHECK(raw_balance(&db, tb.acct[0]) == 1000, "data files still show the OLD balance");

    Db fresh;
    if (db_open(&fresh, dir) < 0) die("db_open");     /* restart = recovery */
    CHECK(raw_balance(&fresh, tb.acct[0]) == 600 && raw_balance(&fresh, tb.acct[1]) == 1400,
          "after restart the committed transfer is there (600 / 1400)");
    remove_test_dir(dir);
}

static void ids_never_reused(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    int id1, id2, id3;

    SCENARIO("id counters survive restart and stale counters are repaired");
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 100, &tb);

    CHECK(bank_add_customer(&db, &tb.emp, "x1", "pw", "X1", 5, &id1) == BANK_OK, "create customer x1");
    db_close(&db);
    if (db_open(&db, dir) < 0) die("reopen");
    CHECK(bank_add_customer(&db, &tb.emp, "x2", "pw", "X2", 5, &id2) == BANK_OK, "create customer x2 after reopening");
    CHECK(id2 > id1, "new user id is larger than the old one (not reused)");

    /* Simulate a power cut that lost the counter update: zero the counter record. */
    Meta m;
    memset(&m, 0, sizeof m);
    db_write(&db, F_META, META_COUNTERS, &m);
    db_close(&db);

    if (db_open(&db, dir) < 0) die("reopen 2");        /* db_open repairs the counters */
    CHECK(bank_add_customer(&db, &tb.emp, "x3", "pw", "X3", 5, &id3) == BANK_OK, "create customer x3 after the counters were wiped");
    CHECK(id3 > id2, "id is still larger than every existing one (counters repaired from file sizes)");
    User u;
    CHECK(bank_authenticate(&db, "x1", "pw", &u) == BANK_OK && u.id == id1, "old customer x1 was not overwritten");
    remove_test_dir(dir);
}

int main(void)
{
    printf("DURABILITY TESTS (committed data survives)\n");
    survives_kill();
    committed_work_not_lost();
    ids_never_reused();
    return test_finish("test_durability");
}
