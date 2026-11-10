/*
 * bank_customer.c - everything a CUSTOMER can do:
 *   balance, deposit, withdraw, transfer, apply for loan, feedback.
 *
 * Every function follows the same recipe (see journal.h):
 *   check arguments -> txn_want -> txn_lock -> read + check rules ->
 *   txn_write -> txn_commit
 */
#include <string.h>

#include "bank_internal.h"

/* Deposit (delta > 0) or withdraw (delta < 0) on one account.
 * Both need exactly the same steps, so they share this function. */
static int change_balance(Db *db, int account_id, int64_t delta, int tx_type,
                          int64_t *new_balance)
{
    Txn t;
    Account acc;
    TxnRec tr;
    int rc;

    int tid = (int)db_alloc_id(db, F_TXNS);          /* id of the passbook line */
    if (tid < 0) return BANK_E_IO;

    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, account_id);
    txn_want(&t, F_TXNS, tid);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_ACCOUNTS, account_id, &acc);
    if (acc.id == 0)   { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (!acc.active)   { txn_end(&t); return BANK_E_INACTIVE; }
    if (delta < 0 && acc.balance + delta < 0)            { txn_end(&t); return BANK_E_INSUFFICIENT; }
    if (delta > 0 && acc.balance > INT64_MAX - delta)    { txn_end(&t); return BANK_E_INVALID; }

    acc.balance += delta;
    passbook_add(&acc, &tr, tid, tx_type, 0, delta > 0 ? delta : -delta);

    txn_write(&t, F_ACCOUNTS, account_id, &acc);
    txn_write(&t, F_TXNS, tid, &tr);
    rc = txn_commit(&t);
    if (rc == BANK_OK && new_balance) *new_balance = acc.balance;
    return rc;
}

int bank_balance(Db *db, const User *me, int64_t *balance)
{
    Txn t;
    Account acc;
    int rc;

    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;

    /* READ lock: many customers may read at once; only blocks while a
     * writer is changing this one account. */
    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, me->account_id);
    if ((rc = txn_lock(&t, LK_READ)) != BANK_OK) { txn_end(&t); return rc; }
    txn_read(&t, F_ACCOUNTS, me->account_id, &acc);
    txn_end(&t);

    if (acc.id == 0) return BANK_E_NOT_FOUND;
    *balance = acc.balance;
    return BANK_OK;
}

int bank_deposit(Db *db, const User *me, int64_t amount, int64_t *new_balance)
{
    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (amount <= 0) return BANK_E_INVALID;
    return change_balance(db, me->account_id, amount, TX_DEPOSIT, new_balance);
}

int bank_withdraw(Db *db, const User *me, int64_t amount, int64_t *new_balance)
{
    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (amount <= 0) return BANK_E_INVALID;
    return change_balance(db, me->account_id, -amount, TX_WITHDRAW, new_balance);
}

/* Transfer: TWO accounts change, so we lock both.  txn_lock always locks the
 * lower account id first, so A->B and B->A running at the same time can
 * never deadlock.  The 4 writes (2 balances + 2 passbook lines) are one
 * journal transaction: either all happen or none. */
int bank_transfer(Db *db, const User *me, int to_account, int64_t amount, int64_t *new_balance)
{
    Txn t;
    Account from, to;
    TxnRec tr_out, tr_in;
    int rc;

    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (amount <= 0 || to_account <= 0 || to_account == me->account_id) return BANK_E_INVALID;

    int from_id = me->account_id;
    int tid1 = (int)db_alloc_id(db, F_TXNS);
    int tid2 = (int)db_alloc_id(db, F_TXNS);
    if (tid1 < 0 || tid2 < 0) return BANK_E_IO;

    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, from_id);
    txn_want(&t, F_ACCOUNTS, to_account);
    txn_want(&t, F_TXNS, tid1);
    txn_want(&t, F_TXNS, tid2);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_ACCOUNTS, from_id, &from);
    txn_read(&t, F_ACCOUNTS, to_account, &to);
    if (from.id == 0 || to.id == 0)          { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (!from.active || !to.active)          { txn_end(&t); return BANK_E_INACTIVE; }
    if (from.balance < amount)               { txn_end(&t); return BANK_E_INSUFFICIENT; }
    if (to.balance > INT64_MAX - amount)     { txn_end(&t); return BANK_E_INVALID; }

    from.balance -= amount;
    to.balance   += amount;
    passbook_add(&from, &tr_out, tid1, TX_TRANSFER_OUT, to_account, amount);
    passbook_add(&to,   &tr_in,  tid2, TX_TRANSFER_IN,  from_id,    amount);

    txn_write(&t, F_ACCOUNTS, from_id, &from);
    txn_write(&t, F_ACCOUNTS, to_account, &to);
    txn_write(&t, F_TXNS, tid1, &tr_out);
    txn_write(&t, F_TXNS, tid2, &tr_in);
    rc = txn_commit(&t);
    if (rc == BANK_OK && new_balance) *new_balance = from.balance;
    return rc;
}

/* Apply for a loan: just creates a PENDING loan record.  A manager assigns it
 * to an employee, the employee approves or rejects it (bank_staff.c). */
int bank_apply_loan(Db *db, const User *me, int64_t amount, int *loan_id)
{
    Txn t;
    Loan loan;
    int rc;

    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (amount <= 0) return BANK_E_INVALID;

    int id = (int)db_alloc_id(db, F_LOANS);
    if (id < 0) return BANK_E_IO;

    memset(&loan, 0, sizeof loan);
    loan.id = id;
    loan.account_id = me->account_id;
    loan.customer_id = me->id;
    loan.status = LOAN_PENDING;
    loan.amount = amount;
    loan.time = now_secs();

    /* Lock the account too: it holds the head of this customer's loan chain. */
    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, me->account_id);
    txn_want(&t, F_LOANS, id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }
    Account acc;
    txn_read(&t, F_ACCOUNTS, me->account_id, &acc);
    if (acc.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }
    loan.prev_id = acc.last_loan_id;          /* link to the previous application ... */
    acc.last_loan_id = id;                    /* ... and become the newest one        */
    txn_write(&t, F_ACCOUNTS, me->account_id, &acc);
    txn_write(&t, F_LOANS, id, &loan);
    rc = txn_commit(&t);
    if (rc == BANK_OK && loan_id) *loan_id = id;
    return rc;
}

int bank_add_feedback(Db *db, const User *me, const char *text)
{
    Txn t;
    Feedback fb;
    int rc;

    if (me->role != ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (!text || text[0] == '\0' || strlen(text) >= TEXT_LEN) return BANK_E_INVALID;

    int id = (int)db_alloc_id(db, F_FEEDBACK);
    if (id < 0) return BANK_E_IO;

    memset(&fb, 0, sizeof fb);
    fb.id = id;
    fb.user_id = me->id;
    str_copy(fb.username, sizeof fb.username, me->username);
    fb.time = now_secs();
    str_copy(fb.text, sizeof fb.text, text);

    txn_begin(&t, db);
    txn_want(&t, F_FEEDBACK, id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }
    txn_write(&t, F_FEEDBACK, id, &fb);
    return txn_commit(&t);
}
