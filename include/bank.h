/*
 * bank.h - THE banking API.  This is the only header a network layer needs.
 *
 * Every function:
 *   - takes the Db handle of the calling worker (see db.h),
 *   - takes `me` = the logged-in user (the User you got from bank_login),
 *   - checks that `me` has the right role,
 *   - returns BANK_OK (0) or a negative BANK_E_* code (see common.h).
 *   - never prints and never touches sockets, so any server design can call it.
 *
 * Amounts are whole numbers in the smallest currency unit and must be > 0.
 */
#ifndef BANK_H
#define BANK_H

#include "common.h"
#include "db.h"

/* ---------- first-time setup ---------- */
/* Create the very first administrator (does nothing if users already exist). */
int bank_create_first_admin(Db *db, const char *username, const char *password);

/* ---------- login / sessions (all roles) ---------- */
/* Check username+password.  Fills *out.  No session lock is taken. */
int  bank_authenticate(Db *db, const char *username, const char *password, User *out);

/* "One session per user": take / give back a session lock for user_id.
 * session_acquire returns a file descriptor (>= 0) that MUST be kept open
 * for the whole session, or BANK_E_ALREADY_LOGGED_IN.  If the process that
 * holds the fd dies, the kernel frees the session automatically. */
int  session_acquire(Db *db, int user_id);
/* Same, but only needs the data directory (the server loop has no Db). */
int  session_acquire_dir(const char *dir, int user_id);
void session_release(int session_fd);

/* Convenience: authenticate + session_acquire in one call. */
int  bank_login(Db *db, const char *username, const char *password,
                User *out, int *session_fd);

/* ---------- any role ---------- */
int  bank_change_password(Db *db, const User *me, const char *new_password);

/* Look up a user by id (staff only). The password hash is blanked in *out. */
int  bank_get_user(Db *db, const User *me, int user_id, User *out);

/* Re-read a user's record (so deactivation / role changes apply at once).
 * BANK_E_INACTIVE if the user was deactivated. */
int  bank_refresh_user(Db *db, int user_id, User *out);

/* Staff only: find a user by username. The password hash is blanked. */
int  bank_find_user(Db *db, const User *me, const char *username, User *out);

/* ---------- paged lists (used by the network server) ----------
 * Each returns up to `max` items whose id is > after_id, oldest first.
 * *more = 1 if there are further items (call again with after_id = last id). */
int  bank_history_page(Db *db, const User *me, int account_id, int after_id,
                       TxnRec *out, int max, int *count, int *more);
int  bank_my_loans_page(Db *db, const User *me, int after_id,
                        Loan *out, int max, int *count, int *more);
int  bank_assigned_loans_page(Db *db, const User *me, int after_id,
                              Loan *out, int max, int *count, int *more);   /* employee */
int  bank_pending_loans_page(Db *db, const User *me, int after_id,
                             Loan *out, int max, int *count, int *more);    /* manager  */
int  bank_feedback_page(Db *db, const User *me, int after_id,
                        Feedback *out, int max, int *count, int *more);     /* manager  */

/* ---------- customer ---------- */
int  bank_balance (Db *db, const User *me, int64_t *balance);
int  bank_deposit (Db *db, const User *me, int64_t amount, int64_t *new_balance);
int  bank_withdraw(Db *db, const User *me, int64_t amount, int64_t *new_balance);
int  bank_transfer(Db *db, const User *me, int to_account, int64_t amount, int64_t *new_balance);
int  bank_apply_loan(Db *db, const User *me, int64_t amount, int *loan_id);
int  bank_add_feedback(Db *db, const User *me, const char *text);

/* Passbook.  Customers may only read their own account; staff may read any.
 * `out` receives up to `max` of the most recent lines (oldest first). */
int  bank_history(Db *db, const User *me, int account_id,
                  TxnRec *out, int max, int *count);

/* ---------- employee ---------- */
int  bank_add_customer(Db *db, const User *me, const char *username, const char *password,
                       const char *name, int64_t initial_deposit, int *user_id);
int  bank_modify_user(Db *db, const User *me, int user_id, const char *new_name);
int  bank_list_assigned_loans(Db *db, const User *me, Loan *out, int max, int *count);
int  bank_process_loan(Db *db, const User *me, int loan_id, int approve);

/* ---------- manager ---------- */
int  bank_set_account_active(Db *db, const User *me, int account_id, int active);
int  bank_assign_loan(Db *db, const User *me, int loan_id, int employee_id);
int  bank_list_feedback(Db *db, const User *me, Feedback *out, int max, int *count);

/* ---------- administrator ---------- */
int  bank_add_employee(Db *db, const User *me, const char *username, const char *password,
                       const char *name, int role, int *user_id);   /* role: EMPLOYEE or MANAGER */
int  bank_set_role(Db *db, const User *me, int user_id, int new_role);

#endif /* BANK_H */
