/*
 * bank_lists.c - "give me the next page" versions of the list functions.
 *
 * A network reply holds only a few items, so long lists are sent in pages.
 * The client says "I have seen everything up to id X" (after_id) and we
 * return the next few items with id > X.  Ids only grow, so this is stable
 * even while new records are being added.
 *
 * Every function here has the same shape:
 *    scan records after_id+1 ... end, keep the ones that match, stop when
 *    the page is full, and look ahead for ONE more match to set *more.
 */
#include <string.h>

#include "bank_internal.h"

/* Passbook page, NEWEST FIRST.  We follow the account's chain of lines, so the cost
 * is one record read per line shown - it does not depend on how big txns.dat is.
 *   after_id = 0        first page: start at the newest line
 *   after_id = line id  start at the line BEFORE that one (the last line of the previous page)
 * The account is read-locked so the page is consistent. */
int bank_history_page(Db *db, const User *me, int account_id, int after_id,
                      TxnRec *out, int max, int *count, int *more)
{
    Txn t;
    TxnRec r;
    Account acc;
    int rc, n = 0, id;

    *count = 0; *more = 0;
    if (max <= 0 || after_id < 0) return BANK_E_INVALID;
    if (me->role == ROLE_CUSTOMER && account_id != me->account_id) return BANK_E_PERMISSION;

    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, account_id);
    if ((rc = txn_lock(&t, LK_READ)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_ACCOUNTS, account_id, &acc);
    if (acc.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }

    if (after_id == 0) {
        id = acc.last_txn_id;
    } else {                                      /* the cursor must be a line of THIS account */
        if (db_read(db, F_TXNS, after_id, &r) < 0) { txn_end(&t); return BANK_E_IO; }
        if (r.id != after_id || r.account_id != account_id) { txn_end(&t); return BANK_E_INVALID; }
        id = r.prev_id;
    }

    while (id != 0) {
        if (n == max) { *more = 1; break; }       /* a further line exists => more pages */
        if (db_read(db, F_TXNS, id, &r) < 0) { txn_end(&t); return BANK_E_IO; }
        if (r.id != id || r.account_id != account_id) { txn_end(&t); return BANK_E_IO; }   /* broken chain */
        out[n++] = r;
        id = r.prev_id;
    }
    txn_end(&t);
    *count = n;
    return BANK_OK;
}

/* Loans of one customer (their own), NEWEST FIRST, by following the chain that starts
 * in the customer's account.  Cost = one read per loan shown.
 *   after_id = 0 first page, else the id of the last loan of the previous page.
 * The account is read-locked (this also finishes any dead worker's commit first). */
int bank_my_loans_page(Db *db, const User *me, int after_id,
                       Loan *out, int max, int *count, int *more)
{
    Txn t;
    Account acc;
    Loan l;
    int rc, n = 0, id;

    *count = 0; *more = 0;
    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (max <= 0 || after_id < 0) return BANK_E_INVALID;

    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, me->account_id);
    if ((rc = txn_lock(&t, LK_READ)) != BANK_OK) { txn_end(&t); return rc; }
    txn_read(&t, F_ACCOUNTS, me->account_id, &acc);
    if (acc.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }

    if (after_id == 0) {
        id = acc.last_loan_id;
    } else {                                      /* the cursor must be a loan of THIS account */
        if (db_read(db, F_LOANS, after_id, &l) < 0) { txn_end(&t); return BANK_E_IO; }
        if (l.id != after_id || l.account_id != me->account_id) { txn_end(&t); return BANK_E_INVALID; }
        id = l.prev_id;
    }
    while (id != 0) {
        if (n == max) { *more = 1; break; }
        if (db_read(db, F_LOANS, id, &l) < 0) { txn_end(&t); return BANK_E_IO; }
        if (l.id != id || l.account_id != me->account_id) { txn_end(&t); return BANK_E_IO; }  /* broken chain */
        out[n++] = l;
        id = l.prev_id;
    }
    txn_end(&t);
    *count = n;
    return BANK_OK;
}

/* Loans assigned to this employee that wait for a decision. */
int bank_assigned_loans_page(Db *db, const User *me, int after_id,
                             Loan *out, int max, int *count, int *more)
{
    Loan l;
    int n = 0;

    *count = 0; *more = 0;
    if (me->role != ROLE_EMPLOYEE) return BANK_E_PERMISSION;
    if (max <= 0 || after_id < 0) return BANK_E_INVALID;

    int total = db_count(db, F_LOANS);
    for (int i = after_id + 1; i < total; i++) {
        if (db_read(db, F_LOANS, i, &l) < 0) return BANK_E_IO;
        if (l.id == 0 || l.employee_id != me->id || l.status != LOAN_ASSIGNED) continue;
        if (n == max) { *more = 1; break; }
        out[n++] = l;
    }
    *count = n;
    return BANK_OK;
}

/* Loans nobody has taken yet (manager screen, to assign them). */
int bank_pending_loans_page(Db *db, const User *me, int after_id,
                            Loan *out, int max, int *count, int *more)
{
    Loan l;
    int n = 0;

    *count = 0; *more = 0;
    if (me->role != ROLE_MANAGER) return BANK_E_PERMISSION;
    if (max <= 0 || after_id < 0) return BANK_E_INVALID;

    int total = db_count(db, F_LOANS);
    for (int i = after_id + 1; i < total; i++) {
        if (db_read(db, F_LOANS, i, &l) < 0) return BANK_E_IO;
        if (l.id == 0 || l.status != LOAN_PENDING) continue;
        if (n == max) { *more = 1; break; }
        out[n++] = l;
    }
    *count = n;
    return BANK_OK;
}

/* Customer feedback (manager only). */
int bank_feedback_page(Db *db, const User *me, int after_id,
                       Feedback *out, int max, int *count, int *more)
{
    Feedback f;
    int n = 0;

    *count = 0; *more = 0;
    if (me->role != ROLE_MANAGER) return BANK_E_PERMISSION;
    if (max <= 0 || after_id < 0) return BANK_E_INVALID;

    int total = db_count(db, F_FEEDBACK);
    for (int i = after_id + 1; i < total; i++) {
        if (db_read(db, F_FEEDBACK, i, &f) < 0) return BANK_E_IO;
        if (f.id == 0) continue;
        if (n == max) { *more = 1; break; }
        out[n++] = f;
    }
    *count = n;
    return BANK_OK;
}
