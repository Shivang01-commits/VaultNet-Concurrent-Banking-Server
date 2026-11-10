/*
 * bank_internal.h - small helpers shared by the bank_*.c files.
 * (Not part of the public API; network layers should not include this.)
 */
#ifndef BANK_INTERNAL_H
#define BANK_INTERNAL_H

#include <stddef.h>
#include "bank.h"
#include "journal.h"

int64_t now_secs(void);

/* copy a C string into a fixed char array, always NUL-terminated */
void str_copy(char *dst, size_t size, const char *src);

/* Read a user by id / by username (no locking, see comments in bank_core.c).
 * Return BANK_OK or BANK_E_NOT_FOUND. */
int user_load(Db *db, int user_id, User *out);
int user_find_by_name(Db *db, const char *username, User *out);

/* Create a user (and, for customers, an account) in one transaction. */
int create_user(Db *db, const char *username, const char *password, const char *name,
                int role, int64_t initial_deposit, int *user_id);

/* Add a passbook line to `acc` (call AFTER acc->balance was updated, while the
 * account is write-locked).  Fills *r, links it to the account's previous line and
 * makes it the account's newest line.  The caller must write BOTH *acc and *r in
 * the same transaction. */
void passbook_add(Account *acc, TxnRec *r, int line_id, int type, int other_account, int64_t amount);

/* ---- username index (bank_names.c) ---- */
uint32_t name_hash(const char *username);

/* Look `username` up in the index.
 *   returns BANK_OK and *user_id, or BANK_E_NOT_FOUND.
 *   *free_bucket (may be NULL) = first empty bucket on its probe path, or -1 if the
 *   table is too full there.  Reads without locking (see bank_names.c). */
int names_find(Db *db, const char *username, int *user_id, int *free_bucket);

/* The bucket record that adds username -> user_id (to be written in a transaction). */
void names_make_slot(NameSlot *slot, const char *username, int user_id);

#endif
