/*
 * bank_core.c - things every role needs: error text, finding users,
 *               login + one-session-per-user, password change, passbook,
 *               and the shared "create a user" routine.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auth.h"
#include "bank_internal.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

const char *bank_strerror(int code)
{
    switch (code) {
    case BANK_OK:                  return "ok";
    case BANK_E_IO:                return "disk or system error";
    case BANK_E_NOT_FOUND:         return "not found";
    case BANK_E_BAD_PASSWORD:      return "wrong username or password";
    case BANK_E_PERMISSION:        return "permission denied for this role";
    case BANK_E_INSUFFICIENT:      return "insufficient funds";
    case BANK_E_INACTIVE:          return "account or user is deactivated";
    case BANK_E_ALREADY_LOGGED_IN: return "user is already logged in elsewhere";
    case BANK_E_EXISTS:            return "username already exists";
    case BANK_E_INVALID:           return "invalid argument";
    case BANK_E_STATE:             return "not allowed in the current state";
    case BANK_E_TOO_MANY:          return "internal limit reached";
    }
    return "unknown error";
}

int64_t now_secs(void) { return (int64_t)time(NULL); }

void str_copy(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);        /* snprintf always adds the final '\0' */
}

/* Add one line to an account's passbook.  The lines of an account form a chain:
 * account.last_txn_id -> newest line -> (prev_id) -> older line -> ... -> 0.
 * The caller holds the account's WRITE lock and writes *acc and *r in ONE transaction,
 * so the chain can never be seen half-linked (not even after a crash). */
void passbook_add(Account *acc, TxnRec *r, int line_id, int type, int other_account, int64_t amount)
{
    memset(r, 0, sizeof *r);
    r->id            = line_id;
    r->account_id    = acc->id;
    r->type          = type;
    r->other_account = other_account;
    r->amount        = amount;
    r->balance_after = acc->balance;          /* call after the balance was updated */
    r->time          = now_secs();
    r->prev_id       = acc->last_txn_id;      /* point to the old newest line ...   */
    acc->last_txn_id = line_id;               /* ... and become the newest line     */
}

/* ------------------------------------------------------------------ */
/* Finding users                                                       */
/* ------------------------------------------------------------------ */

/* These two read WITHOUT locking.  That is fine because:
 *  - usernames never change once created,
 *  - a user record is written in one 144-byte pwrite().
 * Every real decision is re-checked later under a proper record lock. */
int user_load(Db *db, int user_id, User *out)
{
    if (user_id <= 0) return BANK_E_NOT_FOUND;
    if (db_read(db, F_USERS, user_id, out) < 0) return BANK_E_IO;
    return (out->id == user_id) ? BANK_OK : BANK_E_NOT_FOUND;
}

/* Username -> user, through the hash index (bank_names.c): normally one record
 * read for the index bucket and one for the user, however many users exist. */
int user_find_by_name(Db *db, const char *username, User *out)
{
    int uid, rc;
    if ((rc = names_find(db, username, &uid, NULL)) != BANK_OK) return rc;
    return user_load(db, uid, out);
}

/* ------------------------------------------------------------------ */
/* Creating users (used by add_customer, add_employee, first admin)    */
/* ------------------------------------------------------------------ */
int create_user(Db *db, const char *username, const char *password, const char *name,
                int role, int64_t initial_deposit, int *user_id)
{
    User u;
    NameSlot slot;
    Account acc;
    TxnRec tr;
    Txn t;
    int rc;

    if (!username || !password || !name || username[0] == '\0' || password[0] == '\0')
        return BANK_E_INVALID;
    if (strlen(username) >= NAME_LEN || strlen(name) >= NAME_LEN) return BANK_E_INVALID;
    if (role < ROLE_CUSTOMER || role > ROLE_ADMIN) return BANK_E_INVALID;
    if (initial_deposit < 0 || (role != ROLE_CUSTOMER && initial_deposit != 0)) return BANK_E_INVALID;

    /* Build the record outside the lock (hashing is slow, do not hold locks for it). */
    memset(&u, 0, sizeof u);
    if (auth_hash_password(password, u.pass) < 0) return BANK_E_IO;

    int uid = (int)db_alloc_id(db, F_USERS);
    int aid = (role == ROLE_CUSTOMER) ? (int)db_alloc_id(db, F_ACCOUNTS) : 0;
    int tid = (aid && initial_deposit > 0) ? (int)db_alloc_id(db, F_TXNS) : 0;
    if (uid < 0 || aid < 0 || tid < 0) return BANK_E_IO;

    /* Which index bucket will hold the new name?  We look BEFORE locking (a hint),
     * lock that bucket together with everything else, and look AGAIN under the lock.
     * Only one user is created at a time (META_USER_CREATE), so if the second look
     * gives the same bucket it is really free and stays free. */
    int bucket, found_id;
    for (int attempt = 0; ; attempt++) {
        if (attempt == 8) return BANK_E_TOO_MANY;
        rc = names_find(db, username, &found_id, &bucket);
        if (rc == BANK_OK) return BANK_E_EXISTS;
        if (rc != BANK_E_NOT_FOUND) return rc;
        if (bucket < 0) return BANK_E_TOO_MANY;            /* index too full here */

        txn_begin(&t, db);
        txn_want(&t, F_META, META_USER_CREATE);       /* "one user is created at a time" lock */
        txn_want(&t, F_USERS, uid);
        txn_want(&t, F_NAMES, bucket);
        if (aid) txn_want(&t, F_ACCOUNTS, aid);
        if (tid) txn_want(&t, F_TXNS, tid);
        if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

        int bucket2;
        rc = names_find(db, username, &found_id, &bucket2);
        if (rc == BANK_OK)        { txn_end(&t); return BANK_E_EXISTS; }
        if (rc != BANK_E_NOT_FOUND) { txn_end(&t); return rc; }
        if (bucket2 == bucket) break;                       /* locked the right bucket */
        txn_end(&t);                                        /* someone inserted meanwhile: retry */
    }

    u.id = uid;
    str_copy(u.username, sizeof u.username, username);
    str_copy(u.name, sizeof u.name, name);
    u.role = role;
    u.active = 1;
    u.account_id = aid;
    txn_write(&t, F_USERS, uid, &u);
    names_make_slot(&slot, username, uid);
    txn_write(&t, F_NAMES, bucket, &slot);

    if (aid) {
        memset(&acc, 0, sizeof acc);
        acc.id = aid;
        acc.user_id = uid;
        acc.active = 1;
        acc.balance = initial_deposit;
        txn_write(&t, F_ACCOUNTS, aid, &acc);
    }
    if (tid) {
        passbook_add(&acc, &tr, tid, TX_DEPOSIT, 0, initial_deposit);
        txn_write(&t, F_ACCOUNTS, aid, &acc);       /* rewrite: it now knows its first line */
        txn_write(&t, F_TXNS, tid, &tr);
    }

    rc = txn_commit(&t);
    if (rc == BANK_OK && user_id) *user_id = uid;
    return rc;
}

int bank_create_first_admin(Db *db, const char *username, const char *password)
{
    if (db_count(db, F_USERS) > 1) return BANK_OK;      /* already set up */
    return create_user(db, username, password, "Administrator", ROLE_ADMIN, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Login and sessions                                                  */
/* ------------------------------------------------------------------ */

int bank_authenticate(Db *db, const char *username, const char *password, User *out)
{
    User u;
    if (!username || !password) return BANK_E_INVALID;
    /* Same error for "no such user" and "wrong password": do not tell attackers which. */
    if (user_find_by_name(db, username, &u) != BANK_OK) return BANK_E_BAD_PASSWORD;
    if (!auth_check_password(password, u.pass))         return BANK_E_BAD_PASSWORD;
    if (!u.active)                                      return BANK_E_INACTIVE;
    *out = u;
    return BANK_OK;
}

/* One session per user.
 *
 * We keep a 1-byte lock in sessions.dat at position `user_id` for as long as
 * the user is logged in.  We open the file again for every session on purpose:
 * the lock belongs to THAT file descriptor, so closing it (logout) or the
 * process dying (crash) releases the lock - no stale "logged in" flags. */
int session_acquire_dir(const char *dir, int user_id)
{
    char path[300];
    snprintf(path, sizeof path, "%s/sessions.dat", dir);

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return BANK_E_IO;

    if (db_lock_range(fd, user_id, 1, LK_WRITE, 0) < 0) {   /* wait = 0: never sleep */
        close(fd);
        return (errno == EAGAIN || errno == EACCES) ? BANK_E_ALREADY_LOGGED_IN : BANK_E_IO;
    }
    return fd;
}

int session_acquire(Db *db, int user_id)
{
    return session_acquire_dir(db->dir, user_id);
}

void session_release(int session_fd)
{
    if (session_fd >= 0) close(session_fd);   /* closing drops the lock */
}

int bank_login(Db *db, const char *username, const char *password,
               User *out, int *session_fd)
{
    int rc = bank_authenticate(db, username, password, out);
    if (rc != BANK_OK) return rc;

    int fd = session_acquire(db, out->id);
    if (fd < 0) return fd;                    /* BANK_E_ALREADY_LOGGED_IN or IO */
    *session_fd = fd;
    return BANK_OK;
}

/* Re-read the user's record before serving a request, so a deactivation or
 * role change made by an admin takes effect on the very next request. */
int bank_refresh_user(Db *db, int user_id, User *out)
{
    int rc = user_load(db, user_id, out);
    if (rc != BANK_OK) return rc;
    if (!out->active) return BANK_E_INACTIVE;
    return BANK_OK;
}

/* ------------------------------------------------------------------ */
/* Change password (every role)                                        */
/* ------------------------------------------------------------------ */
int bank_change_password(Db *db, const User *me, const char *new_password)
{
    User u;
    Txn t;
    char hash[PASS_LEN];
    int rc;

    if (!new_password || new_password[0] == '\0') return BANK_E_INVALID;
    if (auth_hash_password(new_password, hash) < 0) return BANK_E_IO;

    txn_begin(&t, db);
    txn_want(&t, F_USERS, me->id);
    if ((rc = txn_lock(&t, LK_WRITE)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_USERS, me->id, &u);
    if (u.id != me->id) { txn_end(&t); return BANK_E_NOT_FOUND; }
    str_copy(u.pass, sizeof u.pass, hash);
    txn_write(&t, F_USERS, me->id, &u);
    return txn_commit(&t);
}

/* Staff can look a user up by id (for example to learn a customer's account id). */
int bank_get_user(Db *db, const User *me, int user_id, User *out)
{
    if (me->role == ROLE_CUSTOMER) return BANK_E_PERMISSION;
    int rc = user_load(db, user_id, out);
    if (rc == BANK_OK) out->pass[0] = '\0';        /* never hand out the hash */
    return rc;
}

/* Staff can look a user up by username (to learn ids). Hash is blanked. */
int bank_find_user(Db *db, const User *me, const char *username, User *out)
{
    if (me->role == ROLE_CUSTOMER) return BANK_E_PERMISSION;
    if (!username || username[0] == '\0') return BANK_E_INVALID;
    int rc = user_find_by_name(db, username, out);
    if (rc == BANK_OK) out->pass[0] = '\0';
    return rc;
}

/* ------------------------------------------------------------------ */
/* Passbook (view transaction history)                                 */
/* ------------------------------------------------------------------ */
int bank_history(Db *db, const User *me, int account_id,
                 TxnRec *out, int max, int *count)
{
    Txn t;
    Account acc;
    int rc, n = 0;

    if (max <= 0) return BANK_E_INVALID;
    /* customers: only their own account.  staff: any account. */
    if (me->role == ROLE_CUSTOMER && account_id != me->account_id) return BANK_E_PERMISSION;

    /* READ lock on the account: nobody can add a line while we walk the chain. */
    txn_begin(&t, db);
    txn_want(&t, F_ACCOUNTS, account_id);
    if ((rc = txn_lock(&t, LK_READ)) != BANK_OK) { txn_end(&t); return rc; }

    txn_read(&t, F_ACCOUNTS, account_id, &acc);
    if (acc.id == 0) { txn_end(&t); return BANK_E_NOT_FOUND; }

    /* Walk from the newest line backwards (at most `max` lines) ... */
    for (int id = acc.last_txn_id; id != 0 && n < max; ) {
        TxnRec r;
        if (db_read(db, F_TXNS, id, &r) < 0) { txn_end(&t); return BANK_E_IO; }
        if (r.id != id || r.account_id != account_id) { txn_end(&t); return BANK_E_IO; }   /* broken chain */
        out[n++] = r;
        id = r.prev_id;
    }
    txn_end(&t);

    /* ... and give them back oldest first. */
    for (int i = 0, j = n - 1; i < j; i++, j--) { TxnRec x = out[i]; out[i] = out[j]; out[j] = x; }
    *count = n;
    return BANK_OK;
}
