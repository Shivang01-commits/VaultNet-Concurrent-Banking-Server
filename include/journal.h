/*
 * journal.h - transactions: "all of these writes happen, or none of them".
 *
 * HOW A BANKING FUNCTION USES IT  (every bank_* function follows this recipe)
 *
 *      Txn t;
 *      txn_begin(&t, db);
 *      txn_want(&t, F_ACCOUNTS, from_id);     // 1. say which records we need
 *      txn_want(&t, F_ACCOUNTS, to_id);
 *      txn_lock(&t, LK_WRITE);              // 2. lock them (in a safe order)
 *      txn_read(&t, F_ACCOUNTS, from_id, &a); // 3. read, check the rules
 *      ...
 *      txn_write(&t, F_ACCOUNTS, from_id, &a);// 4. stage the new values (memory only)
 *      txn_write(&t, F_ACCOUNTS, to_id, &b);
 *      txn_commit(&t);                        // 5. make it permanent + unlock
 *
 *   If a rule fails (e.g. not enough money) call txn_end(&t) instead of
 *   txn_commit(): nothing was written, we just unlock.
 *
 * THE JOURNAL IN FIVE LINES  (a "redo" journal)
 *   1. Write the NEW values of all records into our journal slot  -> fsync
 *   2. Set the flag COMMITTED in the (memory-mapped) journal header -> msync
 *      ===== this is the COMMIT POINT: before it nothing happened,
 *            after it the whole transaction is guaranteed =====
 *   3. Copy the new values into the real data files                -> fsync
 *   4. Write the flag EMPTY into the slot
 *   5. Unlock the records
 *   If the worker dies anywhere in 1-5, the next worker that touches one of
 *   the records finds the slot, sees COMMITTED, and simply repeats step 3
 *   (repeating is harmless: it writes the same bytes again).
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include "db.h"

#define MAX_WRITES 8     /* a transaction may change at most 8 records */
#define MAX_LOCKS  8     /* ... and lock at most 8 records             */

/* Slot states stored in the first 4 bytes of a journal slot */
enum { J_EMPTY = 0, J_COMMITTED = 1 };

/* One staged write: "put these bytes into record `index` of `file`" */
typedef struct {
    int32_t file;
    int32_t index;
    uint8_t data[MAX_REC];
} JWrite;

/* What one journal slot body contains on disk.
 * (The COMMITTED / EMPTY flag itself lives in the mapped journal header,
 *  see journal.c - the `state` field here is not used.) */
typedef struct {
    int32_t state;               /* unused, kept for layout simplicity       */
    int32_t nwrites;
    JWrite  writes[MAX_WRITES];
} JEntry;

_Static_assert(sizeof(JEntry) <= JSLOT_SIZE, "JEntry must fit in a journal slot");

/* A record we want to lock */
typedef struct { int file; int index; } RecId;

/* One running transaction (lives on the caller's stack) */
typedef struct {
    Db    *db;
    RecId  locks[MAX_LOCKS];
    int    nlocks;
    int    mode;                 /* LK_READ or LK_WRITE                  */
    int    locked;               /* 1 while we hold the record locks         */
    JEntry entry;                /* the staged writes                        */
} Txn;

/* ---- the transaction API (used by bank_*.c) ---- */
void txn_begin (Txn *t, Db *db);
int  txn_want  (Txn *t, int file, int index);               /* register a record to lock */
int  txn_lock  (Txn *t, int mode);                          /* lock all wanted records   */
int  txn_read  (Txn *t, int file, int index, void *buf);
int  txn_write (Txn *t, int file, int index, const void *buf);
int  txn_commit(Txn *t);                                    /* make permanent, then unlock */
void txn_end   (Txn *t);                                    /* just unlock (abort / read-only) */

/* ---- used by db.c ---- */
int  journal_claim_slot (Db *db);   /* reserve a slot for this handle        */
int  journal_recover_all(Db *db);   /* finish work of dead workers (startup) */

/* ---- FOR TESTS ONLY ----
 * If set to 1..4 the process kills itself (like `kill -9`) at that step
 * of the next txn_commit():
 *   1 = journal body written, NOT committed yet   (transaction must vanish)
 *   2 = committed, nothing copied to data files   (must be completed)
 *   3 = committed, only the first record copied   (must be completed)
 *   4 = everything copied, slot not cleared       (must not apply twice)
 */
extern int journal_crash_point;

#endif /* JOURNAL_H */
