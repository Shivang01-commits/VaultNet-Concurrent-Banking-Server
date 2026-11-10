/*
 * test_lists.c - the "page" list functions and the small helpers the server needs.
 *   A. passbook paging: pages are complete, in order, no repeats, `more` is right
 *   B. loans / feedback paging with the right roles
 *   C. bank_refresh_user (deactivation) and bank_find_user
 *   D. session_acquire_dir works without a Db
 */
#include "test_helpers.h"
#include "bank_internal.h"

static void passbook_paging(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    TxnRec page[5];
    int64_t nb;

    SCENARIO("passbook paging (25 deposits, pages of 5 lines)");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 2, 100, &tb);
    for (int i = 0; i < 24; i++) bank_deposit(&db, &tb.cust[0], 1, &nb);
    bank_deposit(&db, &tb.cust[1], 7, &nb);        /* a line of ANOTHER account in between */

    int after = 0, pages = 0, seen = 0, count, more, last = 1 << 30, ordered = 1;
    do {
        int rc = bank_history_page(&db, &tb.cust[0], tb.acct[0], after, page, 5, &count, &more);
        if (rc != BANK_OK) break;
        for (int i = 0; i < count; i++) { if (page[i].id >= last) ordered = 0; last = page[i].id; }
        seen += count;
        if (count) after = page[count - 1].id;
        pages++;
    } while (more);
    CHECK(seen == 25, "all 25 lines (1 opening + 24 deposits) were seen, none of the other account");
    CHECK(pages == 5, "exactly 5 pages of 5 lines");
    CHECK(ordered, "newest first: ids strictly decrease across pages (no repeats)");
    /* cursor must be a line of the SAME account */
    bank_history_page(&db, &tb.emp, tb.acct[1], 0, page, 5, &count, &more);
    int other_line = page[0].id;
    CHECK(bank_history_page(&db, &tb.cust[0], tb.acct[0], other_line, page, 5, &count, &more) == BANK_E_INVALID,
          "cursor pointing at another account's line is refused");
    CHECK(bank_history_page(&db, &tb.cust[0], tb.acct[0], 987654, page, 5, &count, &more) == BANK_E_INVALID,
          "made-up cursor is refused");
    CHECK(bank_history_page(&db, &tb.cust[0], tb.acct[0], 0, page, 5, &count, &more) == BANK_OK
          && count == 5 && more == 1 && page[0].balance_after == 124,
          "first page starts with the newest line (balance 124)");
    CHECK(bank_history_page(&db, &tb.cust[0], tb.acct[1], 0, page, 5, &count, &more) == BANK_E_PERMISSION,
          "a customer cannot page someone else's passbook");
    CHECK(bank_history_page(&db, &tb.emp, tb.acct[1], 0, page, 5, &count, &more) == BANK_OK && count == 2,
          "staff can page any passbook (2 lines)");
    CHECK(bank_history_page(&db, &tb.emp, 99999, 0, page, 5, &count, &more) == BANK_E_NOT_FOUND,
          "unknown account -> not found");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

/* Username index: lookups, duplicates, many users with a tiny chance of clashes. */
static void username_index(void)
{
    char dir[64], name[40];
    Db db;
    TestBank tb;
    User u;
    int uid;

    SCENARIO("username index (hash table in names.dat)");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 1, 100, &tb);
    CHECK(user_find_by_name(&db, "admin", &u) == BANK_OK && u.role == ROLE_ADMIN, "first admin is found by name");
    CHECK(user_find_by_name(&db, "nobody", &u) == BANK_E_NOT_FOUND, "unknown name -> not found");
    CHECK(create_user(&db, "admin", "x", "Dup", ROLE_CUSTOMER, 0, &uid) == BANK_E_EXISTS, "duplicate of admin refused");

    int ok = 1;
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof name, "user%d", i);
        if (create_user(&db, name, "pw", name, ROLE_CUSTOMER, 0, &uid) != BANK_OK) ok = 0;
    }
    CHECK(ok, "2000 users created");
    ok = 1;
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof name, "user%d", i);
        if (user_find_by_name(&db, name, &u) != BANK_OK || strcmp(u.username, name) != 0) ok = 0;
    }
    CHECK(ok, "all 2000 found again by name");
    CHECK(create_user(&db, "user1234", "x", "Dup", ROLE_CUSTOMER, 0, &uid) == BANK_E_EXISTS, "duplicate of user1234 refused");
    CHECK(user_find_by_name(&db, "user2000", &u) == BANK_E_NOT_FOUND, "name that was never created -> not found");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

static void loans_and_feedback(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    Loan loans[4];
    Feedback fb[2];
    int id, count, more;

    SCENARIO("loan and feedback pages");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 2, 100, &tb);
    for (int i = 0; i < 5; i++) bank_apply_loan(&db, &tb.cust[0], 10 + i, &id);
    bank_apply_loan(&db, &tb.cust[1], 99, &id);
    for (int i = 0; i < 3; i++) bank_add_feedback(&db, &tb.cust[0], "hello");

    CHECK(bank_my_loans_page(&db, &tb.cust[0], 0, loans, 4, &count, &more) == BANK_OK && count == 4 && more == 1,
          "customer 0 first page: 4 loans, more = 1");
    CHECK(bank_my_loans_page(&db, &tb.cust[0], loans[3].id, loans, 4, &count, &more) == BANK_OK && count == 1 && more == 0,
          "second page: the last loan, more = 0");
    CHECK(bank_my_loans_page(&db, &tb.cust[0], 0, loans, 4, &count, &more) == BANK_OK
          && loans[0].id == 5 && loans[1].id == 4 && loans[2].id == 3 && loans[3].id == 2,
          "loans come newest first (5, 4, 3, 2)");
    CHECK(bank_my_loans_page(&db, &tb.cust[0], 6, loans, 4, &count, &more) == BANK_E_INVALID,
          "cursor pointing at another customer's loan is refused");
    CHECK(bank_my_loans_page(&db, &tb.cust[1], 0, loans, 4, &count, &more) == BANK_OK && count == 1 && loans[0].id == 6,
          "customer 1 sees only their own loan");
    CHECK(bank_pending_loans_page(&db, &tb.mgr, 0, loans, 4, &count, &more) == BANK_OK && count == 4 && more == 1,
          "manager sees pending loans of everybody (6 -> 4 + more)");
    CHECK(bank_pending_loans_page(&db, &tb.emp, 0, loans, 4, &count, &more) == BANK_E_PERMISSION,
          "employee may not list pending loans");

    bank_assign_loan(&db, &tb.mgr, 1, tb.emp.id);
    CHECK(bank_assigned_loans_page(&db, &tb.emp, 0, loans, 4, &count, &more) == BANK_OK && count == 1 && loans[0].id == 1,
          "employee sees the one loan assigned to them");
    CHECK(bank_feedback_page(&db, &tb.mgr, 0, fb, 2, &count, &more) == BANK_OK && count == 2 && more == 1,
          "feedback page 1: 2 items, more");
    CHECK(bank_feedback_page(&db, &tb.mgr, fb[1].id, fb, 2, &count, &more) == BANK_OK && count == 1 && more == 0,
          "feedback page 2: 1 item, done");
    CHECK(bank_feedback_page(&db, &tb.cust[0], 0, fb, 2, &count, &more) == BANK_E_PERMISSION,
          "customer may not read feedback");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

static void refresh_and_find(void)
{
    char dir[64];
    Db db;
    TestBank tb;
    User u;

    SCENARIO("bank_refresh_user and bank_find_user");
    make_test_dir(dir);
    db_sync_enabled = 0;
    setup_bank(&db, dir, 1, 100, &tb);

    CHECK(bank_refresh_user(&db, tb.cust[0].id, &u) == BANK_OK && u.role == ROLE_CUSTOMER, "refresh returns the current record");
    CHECK(bank_find_user(&db, &tb.emp, "c0", &u) == BANK_OK && u.id == tb.cust[0].id && u.pass[0] == '\0',
          "staff find a user by name; the hash is blanked");
    CHECK(bank_find_user(&db, &tb.cust[0], "c0", &u) == BANK_E_PERMISSION, "customer may not search users");
    CHECK(bank_find_user(&db, &tb.emp, "nobody", &u) == BANK_E_NOT_FOUND, "unknown name -> not found");

    /* role change by admin is visible at once */
    bank_set_role(&db, &tb.admin, tb.emp.id, ROLE_MANAGER);
    CHECK(bank_refresh_user(&db, tb.emp.id, &u) == BANK_OK && u.role == ROLE_MANAGER, "role change is seen by refresh");
    db_sync_enabled = 1;
    remove_test_dir(dir);
}

static void sessions_by_dir(void)
{
    char dir[64];
    Db db;
    TestBank tb;

    SCENARIO("session_acquire_dir (no Db needed)");
    make_test_dir(dir);
    setup_bank(&db, dir, 1, 0, &tb);
    int a = session_acquire_dir(dir, tb.cust[0].id);
    int b = session_acquire_dir(dir, tb.cust[0].id);
    CHECK(a >= 0, "first session lock granted");
    CHECK(b == BANK_E_ALREADY_LOGGED_IN, "second one for the same user refused");
    session_release(a);
    b = session_acquire_dir(dir, tb.cust[0].id);
    CHECK(b >= 0, "after release it is free again");
    session_release(b);
    remove_test_dir(dir);
}

int main(void)
{
    printf("LIST / HELPER TESTS (functions used by the network server)\n");
    passbook_paging();
    username_index();
    loans_and_feedback();
    refresh_and_find();
    sessions_by_dir();
    return test_finish("test_lists");
}
