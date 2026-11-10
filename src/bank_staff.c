/*
 * bank_staff.c - what EMPLOYEES, MANAGERS and ADMINISTRATORS can do.
 *
 *   employee : add customer, modify customer, list assigned loans, approve/reject loan
 *   manager  : activate/deactivate account, assign loan to employee, read feedback
 *   admin    : add employee/manager, change roles, modify any staff/customer
 *
 * Same recipe as bank_customer.c.  Role checks come first, before any lock.
 */
#include <string.h>

#include "bank_internal.h"

/* ================================================================== */
/* EMPLOYEE                                                            */
/* ================================================================== */

int bank_add_customer(Db *db, const User *me, const char *username, const char *password,
                      const char *name, int64_t initial_deposit, int *user_id)
{
    if (me->role != ROLE_EMPLOYEE) return BANK_E_PERMISSION;
    return create_user(db, username, password, name, ROLE_CUSTOMER, initial_deposit, user_id);
}

/* Change the display name of a user.
 *   employee: customers only.     admin: anybody. */
int bank_modify_user(Db *db, const User *me, int user_id, const char *new_name)
{
    Txn t;
    User u;
    int rc;

    if (me->role != ROLE_EMPLOYEE && me->role != ROLE_ADMIN) return BANK_E_PERMISSION;
    if (!new_name || new_name[0] == '\0' || strlen(new_name) >= NAME_LEN) return BANK_E_INVALID;

    txn_begin(&t, db);
    txn_want(&t, F_USERS, user_id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_USERS, user_id, &u);
    if (u.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (me->role == ROLE_EMPLOYEE && u.role != ROLE_CUSTOMER) { txn_end(&t); return BANK_E_PERMISSION; }

    str_copy(u.name, sizeof u.name, new_name);
    txn_write(&t, F_USERS, user_id, &u);
    return txn_commit(&t);
}

/* List loans assigned to this employee that still wait for a decision.
 * (Listing does not lock; approving/rejecting re-checks under a lock.) */
int bank_list_assigned_loans(Db *db, const User *me, Loan *out, int max, int *count)
{
    Loan l;
    int n = 0;

    if (me->role != ROLE_EMPLOYEE) return BANK_E_PERMISSION;
    int total = db_count(db, F_LOANS);
    for (int i = 1; i < total && n < max; i++) {
        if (db_read(db, F_LOANS, i, &l) < 0) return BANK_E_IO;
        if (l.id != 0 && l.employee_id == me->id && l.status == LOAN_ASSIGNED)
            out[n++] = l;
    }
    *count = n;
    return BANK_OK;
}

/* Approve or reject a loan assigned to me.
 * Approve = the loan amount is credited to the customer's account AND the loan
 * is marked APPROVED AND a passbook line is added: 3 records, one transaction. */
int bank_process_loan(Db *db, const User *me, int loan_id, int approve)
{
    Txn t;
    Loan loan, peek;
    Account acc;
    TxnRec tr;
    int rc;

    if (me->role != ROLE_EMPLOYEE) return BANK_E_PERMISSION;
    if (loan_id <= 0) return BANK_E_INVALID;

    /* Peek (no lock) only to learn WHICH account to lock. The real checks
     * are done again after we hold the locks. */
    if (db_read(db, F_LOANS, loan_id, &peek) < 0) return BANK_E_IO;
    if (peek.id == 0) return BANK_E_NOT_FOUND;

    int tid = approve ? (int)db_alloc_id(db, F_TXNS) : 0;

    txn_begin(&t, db);
    txn_want(&t, F_LOANS, loan_id);
    if (approve) {
        txn_want(&t, F_ACCOUNTS, peek.account_id);
        txn_want(&t, F_TXNS, tid);
    }
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_LOANS, loan_id, &loan);
    if (loan.id == 0)                    { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (loan.employee_id != me->id)      { txn_end(&t); return BANK_E_PERMISSION; }
    if (loan.status != LOAN_ASSIGNED)    { txn_end(&t); return BANK_E_STATE; }  /* already decided */

    if (approve) {
        txn_read(&t, F_ACCOUNTS, loan.account_id, &acc);
        if (acc.id == 0 || loan.account_id != peek.account_id) { txn_end(&t); return BANK_E_NOT_FOUND; }
        if (!acc.active)                                       { txn_end(&t); return BANK_E_INACTIVE; }
        if (acc.balance > INT64_MAX - loan.amount)             { txn_end(&t); return BANK_E_INVALID; }

        acc.balance += loan.amount;
        passbook_add(&acc, &tr, tid, TX_LOAN_CREDIT, 0, loan.amount);
        txn_write(&t, F_ACCOUNTS, acc.id, &acc);
        txn_write(&t, F_TXNS, tid, &tr);
        loan.status = LOAN_APPROVED;
    } else {
        loan.status = LOAN_REJECTED;
    }
    txn_write(&t, F_LOANS, loan_id, &loan);
    return txn_commit(&t);
}

/* ================================================================== */
/* MANAGER                                                             */
/* ================================================================== */

/* Freeze (active = 0) or unfreeze (active = 1) a customer's account. */
int bank_set_account_active(Db *db, const User *me, int account_id, int active)
{
    Txn t;
    Account acc;
    int rc;

    if (me->role != ROLE_MANAGER) return BANK_E_PERMISSION;

    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, account_id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_ACCOUNTS, account_id, &acc);
    if (acc.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }
    acc.active = active ? 1 : 0;
    txn_write(&t, F_ACCOUNTS, account_id, &acc);
    return txn_commit(&t);
}

/* Give a pending (or already assigned) loan to an employee. */
int bank_assign_loan(Db *db, const User *me, int loan_id, int employee_id)
{
    Txn t;
    Loan loan;
    User emp;
    int rc;

    if (me->role != ROLE_MANAGER) return BANK_E_PERMISSION;
    if (user_load(db, employee_id, &emp) != BANK_OK) return BANK_E_NOT_FOUND;
    if (emp.role != ROLE_EMPLOYEE || !emp.active) return BANK_E_INVALID;

    txn_begin(&t, db);
    txn_want(&t, F_LOANS, loan_id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_LOANS, loan_id, &loan);
    if (loan.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (loan.status != LOAN_PENDING && loan.status != LOAN_ASSIGNED) { txn_end(&t); return BANK_E_STATE; }

    loan.employee_id = employee_id;
    loan.status = LOAN_ASSIGNED;
    txn_write(&t, F_LOANS, loan_id, &loan);
    return txn_commit(&t);
}

/* Read all feedback (newest `max`). */
int bank_list_feedback(Db *db, const User *me, Feedback *out, int max, int *count)
{
    Feedback fb;
    int n = 0;

    if (me->role != ROLE_MANAGER) return BANK_E_PERMISSION;
    if (max <= 0) return BANK_E_INVALID;
    int total = db_count(db, F_FEEDBACK);
    for (int i = 1; i < total; i++) {
        if (db_read(db, F_FEEDBACK, i, &fb) < 0) return BANK_E_IO;
        if (fb.id == 0) continue;
        if (n == max) { memmove(out, out + 1, sizeof(Feedback) * (max - 1)); n--; }
        out[n++] = fb;
    }
    *count = n;
    return BANK_OK;
}

/* ================================================================== */
/* ADMINISTRATOR                                                       */
/* ================================================================== */

int bank_add_employee(Db *db, const User *me, const char *username, const char *password,
                      const char *name, int role, int *user_id)
{
    if (me->role != ROLE_ADMIN) return BANK_E_PERMISSION;
    if (role != ROLE_EMPLOYEE && role != ROLE_MANAGER) return BANK_E_INVALID;
    return create_user(db, username, password, name, role, 0, user_id);
}

/* Switch a staff member between EMPLOYEE and MANAGER. */
int bank_set_role(Db *db, const User *me, int user_id, int new_role)
{
    Txn t;
    User u;
    int rc;

    if (me->role != ROLE_ADMIN) return BANK_E_PERMISSION;
    if (new_role != ROLE_EMPLOYEE && new_role != ROLE_MANAGER) return BANK_E_INVALID;

    txn_begin(&t, db);
    txn_want(&t, F_USERS, user_id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_USERS, user_id, &u);
    if (u.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }
    if (u.role != ROLE_EMPLOYEE && u.role != ROLE_MANAGER) { txn_end(&t); return BANK_E_PERMISSION; }

    u.role = new_role;
    txn_write(&t, F_USERS, user_id, &u);
    return txn_commit(&t);
}
