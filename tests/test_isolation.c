/*
 * test_isolation.c - "I" of ACID: concurrent requests do not disturb each other.
 *
 *   A. many processes deposit to the SAME account       -> no lost updates
 *   B. opposite transfers A->B and B->A at the same time -> no deadlock
 *   C. many THREADS (each with its own Db) deposit       -> OFD locks work between threads
 *   D. record locks are per record, readers share
 *   E. one session per user (and it is freed when the process dies)
 */
#include <pthread.h>
#include "test_helpers.h"

static void deposits_in_processes(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("8 processes x 150 deposits of 1 into the same account (no lost update)");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 1, 0, &tb);

    fflush(stdout);
    for (int p = 0; p < 8; p++) {
        if (fork() == 0) {
            Db c;
            int64_t nb;
            if (db_open(&c, dir) < 0) _exit(2);
            for (int i = 0; i < 150; i++) bank_deposit(&c, &tb.cust[0], 1, &nb);
            _exit(0);
        }
    }
    int status;
    while (wait(&status) > 0) {}
    CHECK(raw_balance(&db, tb.acct[0]) == 1200, "final balance is exactly 8 x 150 = 1200");
    CHECK(history_matches_balance(&db, &tb), "passbook has exactly 1200 deposit lines worth of money");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

static void opposite_transfers(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("opposite transfers at the same time (deadlock test, 60 s alarm)");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 2, 100000, &tb);

    fflush(stdout);
    for (int p = 0; p < 4; p++) {
        if (fork() == 0) {
            Db c;
            int64_t nb;
            int me = p % 2, other = 1 - me;      /* 2 processes c0->c1, 2 processes c1->c0 */
            alarm(60);                            /* if we deadlock, the alarm kills us */
            if (db_open(&c, dir) < 0) _exit(2);
            for (int i = 0; i < 300; i++)
                bank_transfer(&c, &tb.cust[me], tb.acct[other], 1, &nb);
            _exit(0);
        }
    }
    int status, finished = 0;
    while (wait(&status) > 0) if (WIFEXITED(status) && WEXITSTATUS(status) == 0) finished++;
    CHECK(finished == 4, "all 4 processes finished (no deadlock)");
    CHECK(total_money(&db) == 200000, "total money unchanged");
    CHECK(history_matches_balance(&db, &tb), "passbooks add up");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

/* ---- threads: each thread opens ITS OWN Db handle ---- */
static const char *g_dir;
static User g_user;

static void *thread_main(void *arg)
{
    Db db;
    int64_t nb;
    (void)arg;
    if (db_open(&db, g_dir) < 0) return (void *)1;
    for (int i = 0; i < 150; i++) bank_deposit(&db, &g_user, 1, &nb);
    db_close(&db);
    return NULL;
}

static void deposits_in_threads(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    pthread_t th[4];

    SCENARIO("4 threads of ONE process x 150 deposits (each thread has its own Db)");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 1, 0, &tb);
    g_dir = dir;
    g_user = tb.cust[0];

    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, thread_main, NULL);
    int bad = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); if (r) bad = 1; }
    CHECK(!bad, "threads ran without errors");
    CHECK(raw_balance(&db, tb.acct[0]) == 600, "final balance is exactly 4 x 150 = 600");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

static void lock_behaviour(void)
{
    char dir[64];
    Db a, b;
    TestBank tb;

    SCENARIO("record locks: per record, readers share, writers exclude");
    make_test_dir(dir);
    setup_bank(&a, dir, 2, 1000, &tb);
    if (db_open(&b, dir) < 0) die("db_open b");

    int r0 = tb.acct[0], r1 = tb.acct[1];

    CHECK(db_lock_record(&a, F_ACCOUNTS, r0, LK_WRITE, 0) == 0, "handle A write-locks account 0");
    CHECK(db_lock_record(&b, F_ACCOUNTS, r0, LK_WRITE, 0) < 0, "handle B cannot lock the SAME record");
    CHECK(db_lock_record(&b, F_ACCOUNTS, r0, LK_READ, 0) < 0,  "handle B cannot even read-lock it");
    CHECK(db_lock_record(&b, F_ACCOUNTS, r1, LK_WRITE, 0) == 0, "handle B CAN lock a different record (not a whole-file lock)");
    db_unlock_record(&b, F_ACCOUNTS, r1);
    db_unlock_record(&a, F_ACCOUNTS, r0);

    CHECK(db_lock_record(&a, F_ACCOUNTS, r0, LK_READ, 0) == 0, "A read-locks account 0");
    CHECK(db_lock_record(&b, F_ACCOUNTS, r0, LK_READ, 0) == 0, "B can read-lock it too (readers share)");
    db_unlock_record(&b, F_ACCOUNTS, r0);
    CHECK(db_lock_record(&b, F_ACCOUNTS, r0, LK_WRITE, 0) < 0, "but B cannot write-lock while A reads");
    db_unlock_record(&a, F_ACCOUNTS, r0);
    remove_test_dir(dir);
}

static void one_session_per_user(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    User u;
    int fd1, fd2;

    SCENARIO("one session per user");
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 0, &tb);

    CHECK(bank_login(&db, "c0", "wrong", &u, &fd1) == BANK_E_BAD_PASSWORD, "wrong password is refused");
    CHECK(bank_login(&db, "c0", "pw", &u, &fd1) == BANK_OK, "first login works");
    CHECK(bank_login(&db, "c0", "pw", &u, &fd2) == BANK_E_ALREADY_LOGGED_IN, "second login of the same user is refused");
    session_release(fd1);
    CHECK(bank_login(&db, "c0", "pw", &u, &fd1) == BANK_OK, "after logout the user can log in again");
    session_release(fd1);

    /* A child logs in and is killed: the kernel must free the session. */
    int pipefd[2];
    if (pipe(pipefd) < 0) die("pipe");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Db c;
        User cu;
        int sfd;
        if (db_open(&c, dir) < 0) _exit(2);
        if (bank_login(&c, "c0", "pw", &cu, &sfd) != BANK_OK) _exit(3);
        if (write(pipefd[1], "x", 1) != 1) _exit(4);       /* tell parent: logged in */
        pause();                                            /* stay logged in forever */
        _exit(0);
    }
    char ch;
    if (read(pipefd[0], &ch, 1) != 1) die("child did not log in");
    CHECK(bank_login(&db, "c0", "pw", &u, &fd2) == BANK_E_ALREADY_LOGGED_IN, "user is logged in inside the child process");
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    CHECK(bank_login(&db, "c0", "pw", &u, &fd2) == BANK_OK, "after the child is killed the session is free again");
    session_release(fd2);
    remove_test_dir(dir);
}

int main(void)
{
    printf("ISOLATION TESTS (concurrent requests do not disturb each other)\n");
    deposits_in_processes();
    opposite_transfers();
    deposits_in_threads();
    lock_behaviour();
    one_session_per_user();
    return test_finish("test_isolation");
}
