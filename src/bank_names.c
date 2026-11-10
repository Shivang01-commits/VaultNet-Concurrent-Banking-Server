/*
 * bank_names.c - the username index (a hash table kept in names.dat).
 *
 * WHY: logging in needs "which user has this username?".  Scanning every user
 * record gets slower with every user.  With the index it is one hash and
 * (almost always) one record read.
 *
 * HOW: bucket number = hash(username) % NAME_BUCKETS.  A bucket holds
 * {user id, hash, username}.  If the bucket is taken by another name we look at
 * the next one ("linear probing") until we find the name or an empty bucket.
 * Users are never deleted, so there is nothing to clean up.
 *
 * SAFETY: a new username is added in the SAME transaction (journal) that writes
 * the user record, so the index and the users file can never disagree, even
 * after a crash.  Reading buckets without a lock is fine: one bucket is written
 * with one pwrite().  Two people choosing the same name probe to the SAME empty
 * bucket, and the creator locks that bucket, so only one of them can win.
 */
#include <string.h>

#include "bank_internal.h"

/* FNV-1a: tiny and good enough to spread names over the buckets. */
uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

int names_find(Db *db, const char *username, int *user_id, int *free_bucket)
{
    uint32_t h = name_hash(username);
    int first_free = -1;

    if (free_bucket) *free_bucket = -1;
    if (strlen(username) >= NAME_LEN) return BANK_E_NOT_FOUND;

    for (int step = 0; step < NAME_PROBE_MAX; step++) {
        int idx = (int)((h + (uint32_t)step) % NAME_BUCKETS);
        NameSlot s;
        if (db_read(db, F_NAMES, idx, &s) < 0) return BANK_E_IO;
        if (s.user_id == 0) {                          /* empty bucket: the name is not here */
            first_free = idx;
            break;
        }
        if (s.hash == h && strncmp(s.username, username, NAME_LEN) == 0) {
            if (user_id) *user_id = s.user_id;
            return BANK_OK;
        }
    }
    if (free_bucket) *free_bucket = first_free;
    return BANK_E_NOT_FOUND;
}

void names_make_slot(NameSlot *slot, const char *username, int user_id)
{
    memset(slot, 0, sizeof *slot);
    slot->user_id = user_id;
    slot->hash = name_hash(username);
    str_copy(slot->username, sizeof slot->username, username);
}
