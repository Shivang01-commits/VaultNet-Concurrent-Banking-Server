/*
 * common.h - constants, record layouts and error codes shared by every file.
 *
 * Every table in our "database" is a binary file that holds FIXED-SIZE
 * records one after another:
 *
 *      users.dat     [User][User][User]...
 *      accounts.dat  [Account][Account]...
 *
 * Record number N lives at byte offset  N * sizeof(record).
 * So finding a record never needs a search or an index: just seek.
 *
 * Record id 0 is never used.  A record whose id is 0 means "empty slot".
 * (Brand new / skipped slots are all zero bytes, so they look empty.)
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Roles (from the project requirements)                               */
/* ------------------------------------------------------------------ */
enum {
    ROLE_CUSTOMER = 1,
    ROLE_EMPLOYEE = 2,
    ROLE_MANAGER  = 3,
    ROLE_ADMIN    = 4
};

/* ------------------------------------------------------------------ */
/* The data files.  These numbers are "file ids" used everywhere.      */
/* ------------------------------------------------------------------ */
enum {
    F_USERS = 0,
    F_ACCOUNTS,
    F_TXNS,        /* transaction history (passbook) */
    F_LOANS,
    F_FEEDBACK,
    F_META,        /* record 0 = id counters, record 1 = "create user" lock */
    F_NAMES,       /* username -> user id hash index (see bank_names.c)     */
    F_COUNT        /* how many data files there are */
};

/* Biggest record we ever store.  The journal keeps a copy of records,
 * so it needs to know the maximum size. */
#define MAX_REC 256

/* ------------------------------------------------------------------ */
/* Record layouts                                                      */
/* ------------------------------------------------------------------ */

#define NAME_LEN   32
#define PASS_LEN   64   /* stores the HASH of the password, never the password */
#define TEXT_LEN  128

typedef struct {
    int32_t id;                 /* 0 = empty slot                         */
    char    username[NAME_LEN];
    char    pass[PASS_LEN];     /* salted SHA-256 hash (see auth.c)       */
    char    name[NAME_LEN];     /* full name                              */
    int32_t role;               /* ROLE_*                                 */
    int32_t active;             /* 1 = can log in                         */
    int32_t account_id;         /* customer's account, 0 for staff        */
} User;

typedef struct {
    int32_t id;
    int32_t user_id;            /* owner                                  */
    int32_t active;             /* manager can deactivate (freeze) it     */
    int32_t last_txn_id;        /* NEWEST passbook line of this account (0 = none).
                                 * Lines form a chain: each line points to the one
                                 * before it (TxnRec.prev_id), so a passbook is read
                                 * by following the chain, never by scanning the file. */
    int64_t balance;            /* smallest currency unit                 */
    int32_t last_loan_id;       /* NEWEST loan application of this account (0 = none);
                                 * older ones follow through Loan.prev_id            */
    int32_t pad;
} Account;

/* Transaction types written to the passbook */
enum { TX_DEPOSIT = 1, TX_WITHDRAW, TX_TRANSFER_OUT, TX_TRANSFER_IN, TX_LOAN_CREDIT };

typedef struct {
    int32_t id;
    int32_t account_id;         /* whose passbook line this is            */
    int32_t type;               /* TX_*                                   */
    int32_t other_account;      /* the other side of a transfer, else 0   */
    int64_t amount;
    int64_t balance_after;
    int64_t time;               /* unix seconds                           */
    int32_t prev_id;            /* the previous passbook line of the SAME account (0 = first) */
    int32_t pad;
} TxnRec;

/* Loan states */
enum { LOAN_PENDING = 0, LOAN_ASSIGNED, LOAN_APPROVED, LOAN_REJECTED };

typedef struct {
    int32_t id;
    int32_t account_id;         /* account that will receive the money    */
    int32_t customer_id;        /* user id of the applicant               */
    int32_t status;             /* LOAN_*                                 */
    int32_t employee_id;        /* assigned employee, 0 = nobody yet      */
    int32_t prev_id;            /* previous loan of the SAME account (0 = first) */
    int64_t amount;
    int64_t time;
} Loan;

typedef struct {
    int32_t id;
    int32_t user_id;
    int64_t time;
    char    username[NAME_LEN]; /* who wrote it (user ids are shared by all roles, names are clearer) */
    char    text[TEXT_LEN];
} Feedback;

/* meta.dat record 0: "next free id" for each table (see db_alloc_id) */
typedef struct {
    int64_t next_id[8];
} Meta;

/* meta.dat record 1 is only used as a lock ("only one user is being
 * created at a time", so two people cannot pick the same username). */
#define META_COUNTERS     0
#define META_USER_CREATE  1

/* Username index: a hash table stored as a file.  Record number = bucket.
 * A bucket with user_id 0 is empty.  Lookup = hash the name, look at that
 * bucket, then the next ones until an empty bucket (linear probing). */
#define NAME_BUCKETS     (1 << 18)     /* 262144 buckets (the file is sparse: only used buckets take space) */
#define NAME_PROBE_MAX   1024          /* give up after this many buckets */

typedef struct {
    int32_t  user_id;           /* 0 = empty bucket                       */
    uint32_t hash;              /* hash of the username                   */
    char     username[NAME_LEN];
} NameSlot;

/* If someone edits a struct and it grows too big, fail at compile time. */
_Static_assert(sizeof(User)     <= MAX_REC, "User too big");
_Static_assert(sizeof(Account)  <= MAX_REC, "Account too big");
_Static_assert(sizeof(TxnRec)   <= MAX_REC, "TxnRec too big");
_Static_assert(sizeof(Loan)     <= MAX_REC, "Loan too big");
_Static_assert(sizeof(Feedback) <= MAX_REC, "Feedback too big");
_Static_assert(sizeof(Meta)     <= MAX_REC, "Meta too big");
_Static_assert(sizeof(NameSlot) <= MAX_REC, "NameSlot too big");
_Static_assert(sizeof(TxnRec) == 48, "TxnRec layout changed");

/* ------------------------------------------------------------------ */
/* Error codes: every bank_* function returns BANK_OK (0) or a         */
/* negative number from this list.  bank_strerror() turns it to text.  */
/* ------------------------------------------------------------------ */
enum {
    BANK_OK                 =  0,
    BANK_E_IO               = -1,   /* disk / system call problem         */
    BANK_E_NOT_FOUND        = -2,
    BANK_E_BAD_PASSWORD     = -3,
    BANK_E_PERMISSION       = -4,   /* this role may not do that          */
    BANK_E_INSUFFICIENT     = -5,   /* not enough money                   */
    BANK_E_INACTIVE         = -6,   /* account / user is deactivated      */
    BANK_E_ALREADY_LOGGED_IN = -7,
    BANK_E_EXISTS           = -8,   /* username already taken             */
    BANK_E_INVALID          = -9,   /* bad argument (negative amount...)  */
    BANK_E_STATE            = -10,  /* e.g. loan already processed        */
    BANK_E_TOO_MANY         = -11   /* internal limit reached             */
};

const char *bank_strerror(int code);

#endif /* COMMON_H */
