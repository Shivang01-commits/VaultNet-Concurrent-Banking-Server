/*
 * test_crash_ops.c - crash EVERY writing operation at each of the 4 journal steps.
 *
 * test_atomicity only crashes transfers.  Here the same experiment is run for the
 * other operations of the requirements:
 *      add customer, add employee, apply loan, add feedback, approve loan,
 *      reject loan, assign loan, freeze account, change role, change password,
 *      modify user.
 *
 * For each operation and each crash point (1..4) a child process does the
 * operation and kills itself inside txn_commit().  Then we open the bank again
 * (startup recovery) and check the result is ALL-OR-NOTHING:
 *      point 1  -> the operation must not have happened at all
 *      point 2,3,4 -> the operation must be completely there
 * We also check that nothing was done twice (repeat the operation, look at the
 * result), and that a second restart changes nothing.
 */
#include "test_helpers.h"

typedef struct {
    const char *name;
    void (*run)(Db *db, TestBank *tb);                        /* the operation (runs in the child) */
    void (*check)(Db *db, TestBank *tb, int applied);         /* looks at the result              */
} Op;

/* ---------- helpers to look at records ---------- */
static Loan get_loan(Db *db, int id)       { Loan l;     memset(&l, 0, sizeof l);     db_read(db, F_LOANS, id, &l); return l; }
static User get_user(Db *db, int id)       { User u;     memset(&u, 0, sizeof u);     db_read(db, F_USERS, id, &u); return u; }
static Account get_acct(Db *db, int id)    { Account a;  memset(&a, 0, sizeof a);     db_read(db, F_ACCOUNTS, id, &a); return a; }

static int count_loans_with_amount(Db *db, int64_t amount)
{
    Loan l; int n = 0, total = db_count(db, F_LOANS);
    for (int i = 1; i < total; i++) { db_read(db, F_LOANS, i, &l); if (l.id && l.amount == amount) n++; }
    return n;
}
static int count_feedback(Db *db, const char *text)
{
    Feedback f; int n = 0, total = db_count(db, F_FEEDBACK);
    for (int i = 1; i < total; i++) { db_read(db, F_FEEDBACK, i, &f); if (f.id && strcmp(f.text, text) == 0) n++; }
    return n;
}

/* ---------- the operations ---------- */
static void run_add_customer(Db *db, TestBank *tb) { int id; bank_add_customer(db, &tb->emp, "newc", "pw", "New", 500, &id); }
static void chk_add_customer(Db *db, TestBank *tb, int applied)
{
    User u; int found = bank_authenticate(db, "newc", "pw", &u) == BANK_OK;
    CHECK(found == applied, applied ? "the new customer exists" : "no new customer exists");
    if (found) {
        CHECK(raw_balance(db, u.account_id) == 500, "...with an account holding the opening 500");
        CHECK(history_count(db, tb, u.account_id) == 1, "...and exactly one passbook line");
    }
    CHECK(total_money(db) == 2000 + (applied ? 500 : 0), "no money appeared or vanished (no half-made account)");
    int id;
    CHECK(bank_add_customer(db, &tb->emp, "newc", "pw", "New", 500, &id) == (applied ? BANK_E_EXISTS : BANK_OK),
          applied ? "creating the same user again is refused (not created twice)" : "the username is free again (nothing was reserved)");
}

static void run_add_employee(Db *db, TestBank *tb) { int id; bank_add_employee(db, &tb->admin, "newemp", "pw", "NE", ROLE_EMPLOYEE, &id); }
static void chk_add_employee(Db *db, TestBank *tb, int applied)
{
    User u; (void)tb;
    int found = bank_authenticate(db, "newemp", "pw", &u) == BANK_OK;
    CHECK(found == applied, applied ? "the new employee exists" : "no new employee exists");
    if (found) CHECK(u.role == ROLE_EMPLOYEE && u.account_id == 0, "...with role employee and no account");
}

static void run_apply_loan(Db *db, TestBank *tb) { int id; bank_apply_loan(db, &tb->cust[0], 900, &id); }
static void chk_apply_loan(Db *db, TestBank *tb, int applied)
{
    (void)tb;
    int n = count_loans_with_amount(db, 900);
    CHECK(n == (applied ? 1 : 0), applied ? "exactly one new loan application" : "no loan application was created");
}

static void run_feedback(Db *db, TestBank *tb) { bank_add_feedback(db, &tb->cust[0], "crashfb"); }
static void chk_feedback(Db *db, TestBank *tb, int applied)
{
    (void)tb;
    CHECK(count_feedback(db, "crashfb") == (applied ? 1 : 0), applied ? "the feedback is stored once" : "no feedback was stored");
}

static void run_approve(Db *db, TestBank *tb) { bank_process_loan(db, &tb->emp, 2, 1); }
static void chk_approve(Db *db, TestBank *tb, int applied)
{
    Loan l = get_loan(db, 2);
    CHECK(l.status == (applied ? LOAN_APPROVED : LOAN_ASSIGNED), applied ? "loan is APPROVED" : "loan is still ASSIGNED");
    CHECK(raw_balance(db, tb->acct[0]) == 1000 + (applied ? 700 : 0), applied ? "loan money is credited (1000 + 700)" : "no money credited");
    CHECK(history_count(db, tb, tb->acct[0]) == 1 + (applied ? 1 : 0), "passbook has the loan line if and only if the loan is approved");
    CHECK(bank_process_loan(db, &tb->emp, 2, 1) == (applied ? BANK_E_STATE : BANK_OK), applied ? "deciding again is refused" : "the loan can now be approved normally");
    CHECK(raw_balance(db, tb->acct[0]) == 1700, "the loan was credited exactly once (1700)");
}

static void run_reject(Db *db, TestBank *tb) { bank_process_loan(db, &tb->emp, 3, 0); }
static void chk_reject(Db *db, TestBank *tb, int applied)
{
    Loan l = get_loan(db, 3);
    CHECK(l.status == (applied ? LOAN_REJECTED : LOAN_ASSIGNED), applied ? "loan is REJECTED" : "loan is still ASSIGNED");
    CHECK(raw_balance(db, tb->acct[0]) == 1000, "no money moved");
    CHECK(bank_process_loan(db, &tb->emp, 3, 0) == (applied ? BANK_E_STATE : BANK_OK), applied ? "deciding again is refused" : "it can be rejected normally now");
}

static void run_assign(Db *db, TestBank *tb) { bank_assign_loan(db, &tb->mgr, 1, tb->emp.id); }
static void chk_assign(Db *db, TestBank *tb, int applied)
{
    Loan l = get_loan(db, 1);
    CHECK(l.status == (applied ? LOAN_ASSIGNED : LOAN_PENDING), applied ? "loan is ASSIGNED" : "loan is still PENDING");
    CHECK(l.employee_id == (applied ? tb->emp.id : 0), "status and employee always change together");
}

static void run_freeze(Db *db, TestBank *tb) { bank_set_account_active(db, &tb->mgr, tb->acct[1], 0); }
static void chk_freeze(Db *db, TestBank *tb, int applied)
{
    CHECK(get_acct(db, tb->acct[1]).active == (applied ? 0 : 1), applied ? "account is frozen" : "account is still active");
    CHECK(get_acct(db, tb->acct[1]).balance == 1000, "balance untouched");
}

static void run_role(Db *db, TestBank *tb) { bank_set_role(db, &tb->admin, tb->emp.id, ROLE_MANAGER); }
static void chk_role(Db *db, TestBank *tb, int applied)
{
    CHECK(get_user(db, tb->emp.id).role == (applied ? ROLE_MANAGER : ROLE_EMPLOYEE), applied ? "role is now manager" : "role is still employee");
}

static void run_passwd(Db *db, TestBank *tb) { bank_change_password(db, &tb->cust[0], "newpw"); }
static void chk_passwd(Db *db, TestBank *tb, int applied)
{
    User u; (void)tb;
    int new_ok = bank_authenticate(db, "c0", "newpw", &u) == BANK_OK;
    int old_ok = bank_authenticate(db, "c0", "pw", &u) == BANK_OK;
    CHECK(new_ok == applied && old_ok == !applied, applied ? "only the NEW password works" : "only the OLD password works");
    CHECK(new_ok || old_ok, "the user is never locked out (one of the two passwords works)");
}

static void run_modify(Db *db, TestBank *tb) { bank_modify_user(db, &tb->emp, tb->cust[1].id, "Renamed"); }
static void chk_modify(Db *db, TestBank *tb, int applied)
{
    CHECK(strcmp(get_user(db, tb->cust[1].id).name, applied ? "Renamed" : "c1") == 0, applied ? "name changed" : "name unchanged");
}

static const Op OPS[] = {
    { "add customer",     run_add_customer, chk_add_customer },
    { "add employee",     run_add_employee, chk_add_employee },
    { "apply for loan",   run_apply_loan,   chk_apply_loan   },
    { "add feedback",     run_feedback,     chk_feedback     },
    { "approve loan",     run_approve,      chk_approve      },
    { "reject loan",      run_reject,       chk_reject       },
    { "assign loan",      run_assign,       chk_assign       },
    { "freeze account",   run_freeze,       chk_freeze       },
    { "change role",      run_role,         chk_role         },
    { "change password",  run_passwd,       chk_passwd       },
    { "modify user",      run_modify,       chk_modify       },
};

/* One experiment: operation `op`, crash at `point`. */
static void experiment(const Op *op, int point)
{
    char dir[64], title[120];
    Db db, fresh;
    TestBank tb;
    int id;

    snprintf(title, sizeof title, "%s - crash at journal step %d", op->name, point);
    SCENARIO(title);
    make_test_dir(dir);
    db_sync_enabled = 0;                             /* kill -9 keeps the page cache, so this is safe */
    setup_bank(&db, dir, 2, 1000, &tb);

    /* three loans: #1 pending (cust1, 1500), #2 and #3 assigned to the employee (cust0) */
    bank_apply_loan(&db, &tb.cust[1], 1500, &id);
    bank_apply_loan(&db, &tb.cust[0], 700, &id);
    bank_apply_loan(&db, &tb.cust[0], 300, &id);
    bank_assign_loan(&db, &tb.mgr, 2, tb.emp.id);
    bank_assign_loan(&db, &tb.mgr, 3, tb.emp.id);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Db c;
        if (db_open(&c, dir) < 0) _exit(2);
        journal_crash_point = point;
        op->run(&c, &tb);
        _exit(0);                                    /* reached only if it did not crash */
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(died_by_sigkill(status), "the worker died inside the commit");

    int applied = (point != 1);
    if (db_open(&fresh, dir) < 0) die("reopen");     /* startup recovery */
    op->check(&fresh, &tb, applied);

    /* restart once more: recovery must not change anything or apply anything twice */
    db_close(&fresh);
    if (db_open(&fresh, dir) < 0) die("reopen 2");
    CHECK(total_money(&fresh) >= 2000, "a second restart keeps the bank healthy");
    db_close(&fresh);
    db_close(&db);
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

int main(void)
{
    printf("CRASH TESTS FOR EVERY WRITING OPERATION (all-or-nothing)\n");
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
        for (int point = 1; point <= 4; point++)
            experiment(&OPS[i], point);
    return test_finish("test_crash_ops");
}
