/*
 * journal.c - transactions with a redo journal + crash recovery.
 * Read journal.h first for the big picture.
 *
 * LAYOUT OF journal.dat
 *   [ HEADER (JHEADER_SIZE bytes) ][ slot 0 ][ slot 1 ] ... [ slot 127 ]
 *
 *   HEADER  = one small JHead per slot: "is this slot COMMITTED?" and the ids
 *             of the records it will change.  The header is mapped into memory
 *             (mmap) by every process, so looking at all 128 slots costs about
 *             128 memory reads and ZERO system calls.
 *   SLOT    = JSLOT_SIZE bytes holding the new record values (a JEntry).
 *             Every Db handle (= every worker) owns ONE slot for its whole life.
 *
 * TWO TINY LOCKS PER SLOT (1-byte record locks inside the slot area)
 *   byte 0 of the slot  = "OWNER lock": held by the worker that owns the slot
 *                         for as long as it lives.  If we can take it, the
 *                         owner is dead (the kernel dropped its lock).
 *   byte 1 of the slot  = "REPAIR lock": taken by whoever is repairing the
 *                         slot, so two repairers do not work at the same time.
 *
 * THE ONE IDEA THAT MAKES RECOVERY CORRECT
 *   A worker keeps its record locks until AFTER it has cleared its slot flag.
 *   So if I hold the lock of record R and a slot says "COMMITTED and it
 *   changes R", then the worker that owns that slot must be dead (a live
 *   one would still be holding R).  I then finish its job before I read R.
 *   This needs no "is it dead?" check at all.
 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "journal.h"

int journal_crash_point = 0;

/* ------------------------------------------------------------------ */
/* The mapped header                                                   */
/* ------------------------------------------------------------------ */

/* One per slot.  Lives in shared memory, so it is only accessed with the
 * __atomic helpers (they also make the compiler/CPU keep the order).    */
typedef struct {
    uint32_t seq;        /* even = stable, odd = the owner is writing the ids */
    int32_t  state;      /* J_EMPTY or J_COMMITTED                            */
    int32_t  nwrites;
    int32_t  pad;
    struct { int32_t file; int32_t index; } ids[MAX_WRITES];
} JHead;

_Static_assert(JOURNAL_SLOTS * sizeof(JHead) <= JHEADER_SIZE, "journal header too small");

static JHead *head(Db *db, int slot) { return (JHead *)db->jhead + slot; }

static int head_state(Db *db, int slot)
{
    return __atomic_load_n(&head(db, slot)->state, __ATOMIC_ACQUIRE);
}

/* Take a CONSISTENT copy of a COMMITTED slot's record ids.
 * Returns 1 and fills ids/n if the slot is COMMITTED and its ids did not
 * change while we copied them (a "seqlock" read).  Otherwise returns 0. */
static int snapshot(Db *db, int slot, RecId *ids, int *n)
{
    JHead *h = head(db, slot);

    if (__atomic_load_n(&h->state, __ATOMIC_ACQUIRE) != J_COMMITTED) return 0;
    uint32_t s1 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) return 0;                          /* owner is in the middle of writing */

    int cnt = __atomic_load_n(&h->nwrites, __ATOMIC_RELAXED);
    if (cnt < 0 || cnt > MAX_WRITES) return 0;
    for (int i = 0; i < cnt; i++) {
        ids[i].file  = __atomic_load_n(&h->ids[i].file,  __ATOMIC_RELAXED);
        ids[i].index = __atomic_load_n(&h->ids[i].index, __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&h->seq, __ATOMIC_RELAXED) != s1) return 0;   /* changed: ignore */
    *n = cnt;
    return 1;
}

/* Owner only: publish "my slot is COMMITTED and touches these records".
 * Order matters: ids first (seq odd -> even), the flag LAST. */
static void publish_committed(Db *db, const JEntry *e)
{
    JHead *h = head(db, db->my_slot);
    uint32_t s = h->seq & ~1u;

    __atomic_store_n(&h->seq, s + 1, __ATOMIC_RELEASE);       /* odd: writing   */
    __atomic_thread_fence(__ATOMIC_RELEASE);                  /* keep ids after */
    __atomic_store_n(&h->nwrites, e->nwrites, __ATOMIC_RELAXED);
    for (int i = 0; i < e->nwrites; i++) {
        __atomic_store_n(&h->ids[i].file,  e->writes[i].file,  __ATOMIC_RELAXED);
        __atomic_store_n(&h->ids[i].index, e->writes[i].index, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&h->seq, s + 2, __ATOMIC_RELEASE);       /* even: stable   */
    __atomic_store_n(&h->state, J_COMMITTED, __ATOMIC_RELEASE);
}

static void publish_empty(Db *db, int slot)
{
    __atomic_store_n(&head(db, slot)->state, J_EMPTY, __ATOMIC_RELEASE);
}

/* Make the header page durable (the COMMIT POINT). */
static int flush_header(Db *db)
{
    if (!db_sync_enabled) return 0;
    return msync(db->jhead, JHEADER_SIZE, MS_SYNC);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Test hook: die right here if the test asked us to die at `point`. */
static void maybe_crash(int point)
{
    if (journal_crash_point == point) {
        kill(getpid(), SIGKILL);        /* like `kill -9`: no cleanup at all */
        _exit(137);
    }
}

static off_t slot_offset(int slot) { return (off_t)JHEADER_SIZE + (off_t)slot * JSLOT_SIZE; }

/* Read the body (the new record values) of a slot. */
static void read_slot(Db *db, int slot, JEntry *e)
{
    ssize_t n = pread(db->journal_fd, e, sizeof *e, slot_offset(slot));
    if (n < (ssize_t)sizeof *e) {
        if (n < 0) n = 0;
        memset((char *)e + n, 0, sizeof *e - n);
    }
}

/* Sort record ids by (file, index).  ALWAYS locking in this same order is
 * what prevents deadlock (two workers can never wait for each other). */
static void sort_ids(RecId *ids, int n)
{
    for (int i = 1; i < n; i++) {                /* tiny insertion sort */
        RecId key = ids[i];
        int j = i - 1;
        while (j >= 0 && (ids[j].file > key.file ||
              (ids[j].file == key.file && ids[j].index > key.index))) {
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = key;
    }
}

/* Lock ids[0..n) in order.  If one fails, unlock the ones we got. */
static int lock_all(Db *db, const RecId *ids, int n, int mode)
{
    for (int i = 0; i < n; i++) {
        if (db_lock_record(db, ids[i].file, ids[i].index, mode, 1) < 0) {
            for (int k = 0; k < i; k++) db_unlock_record(db, ids[k].file, ids[k].index);
            return -1;
        }
    }
    return 0;
}

static void unlock_all(Db *db, const RecId *ids, int n)
{
    for (int i = 0; i < n; i++) db_unlock_record(db, ids[i].file, ids[i].index);
}

/* Copy the staged values into the real data files, then fsync those files.
 * Running this twice gives the same result (idempotent) - that is why
 * recovery can safely "redo" it. */
static int apply_entry(Db *db, const JEntry *e, int allow_crash)
{
    int touched[F_COUNT] = {0};

    for (int i = 0; i < e->nwrites; i++) {
        const JWrite *w = &e->writes[i];
        if (db_write(db, w->file, w->index, w->data) < 0) return -1;
        touched[w->file] = 1;
        if (allow_crash && i == 0) maybe_crash(3);   /* test hook: half applied */
    }
    for (int f = 0; f < F_COUNT; f++)
        if (touched[f] && db_fsync(db->fd[f]) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Recovery                                                            */
/* ------------------------------------------------------------------ */

/* Finish the COMMITTED transaction sitting in `slot` (if there still is one).
 *   have_locks = 1: the caller already holds a lock on a record of this
 *                   transaction (lazy recovery from txn_lock).  By the idea
 *                   at the top of this file, the slot's owner is dead.
 *   have_locks = 0: startup recovery.  The caller holds the slot's OWNER
 *                   lock (so the owner is dead) and must lock the records
 *                   itself. */
static void recover_slot(Db *db, int slot, int have_locks)
{
    RecId ids[MAX_WRITES];
    int n = 0;

    if (!snapshot(db, slot, ids, &n)) return;

    if (!have_locks) {
        sort_ids(ids, n);
        if (lock_all(db, ids, n, LK_WRITE) < 0) return;
    }

    /* REPAIR lock (byte 1 of the slot): only one repairer works at a time. */
    db_lock_range(db->journal_fd, slot_offset(slot) + 1, 1, LK_WRITE, 1);
    if (head_state(db, slot) == J_COMMITTED) {   /* someone may have finished it meanwhile */
        JEntry e;
        read_slot(db, slot, &e);
        apply_entry(db, &e, 0);                  /* REDO the writes */
        publish_empty(db, slot);                 /* job done */
    }
    db_unlock_range(db->journal_fd, slot_offset(slot) + 1, 1);

    if (!have_locks) unlock_all(db, ids, n);
}

/* Called from db_open(): repair every slot whose owner is dead. */
int journal_recover_all(Db *db)
{
    for (int s = 0; s < JOURNAL_SLOTS; s++) {
        /* Can we take the OWNER lock?  Only if nobody alive owns the slot. */
        if (db_lock_range(db->journal_fd, slot_offset(s), 1, LK_WRITE, 0) < 0)
            continue;
        if (head_state(db, s) == J_COMMITTED)
            recover_slot(db, s, 0);
        db_unlock_range(db->journal_fd, slot_offset(s), 1);
    }
    return 0;
}

/* Called from db_open(): find a free slot and keep its OWNER lock until we exit. */
int journal_claim_slot(Db *db)
{
    for (int s = 0; s < JOURNAL_SLOTS; s++) {
        if (db_lock_range(db->journal_fd, slot_offset(s), 1, LK_WRITE, 0) < 0)
            continue;                            /* owned by a live worker */
        if (head_state(db, s) == J_COMMITTED) {
            /* A worker died just now and nobody repaired it yet.  Do not
             * use this slot; let a repairer finish it first. */
            db_unlock_range(db->journal_fd, slot_offset(s), 1);
            continue;
        }
        db->my_slot = s;
        return 0;
    }
    return -1;                                   /* all slots are in use */
}

/* ------------------------------------------------------------------ */
/* The transaction API                                                 */
/* ------------------------------------------------------------------ */

void txn_begin(Txn *t, Db *db)
{
    memset(t, 0, sizeof *t);
    t->db = db;
}

/* Remember that we will need record (file,index).  Duplicates are ignored. */
int txn_want(Txn *t, int file, int index)
{
    if (t->locked) return BANK_E_INVALID;        /* too late, already locked */
    for (int i = 0; i < t->nlocks; i++)
        if (t->locks[i].file == file && t->locks[i].index == index) return 0;
    if (t->nlocks >= MAX_LOCKS) return BANK_E_TOO_MANY;
    t->locks[t->nlocks].file  = file;
    t->locks[t->nlocks].index = index;
    t->nlocks++;
    return 0;
}

/* After we hold our locks: look for a dead worker's COMMITTED transaction
 * that touches one of OUR records, and finish it first.
 * Normal case: every flag is EMPTY (or belongs to a live worker working on
 * other records) -> we only read memory and move on. */
static void recover_overlapping(Txn *t)
{
    Db *db = t->db;
    RecId ids[MAX_WRITES];
    int n;

    for (int s = 0; s < JOURNAL_SLOTS; s++) {
        if (s == db->my_slot) continue;
        if (head_state(db, s) != J_COMMITTED) continue;      /* the fast path */
        if (!snapshot(db, s, ids, &n)) continue;
        int overlap = 0;
        for (int i = 0; i < n && !overlap; i++)
            for (int k = 0; k < t->nlocks; k++)
                if (ids[i].file == t->locks[k].file && ids[i].index == t->locks[k].index) {
                    overlap = 1;
                    break;
                }
        if (overlap) recover_slot(db, s, 1);
    }
}

/* Lock every wanted record (smallest first), then repair any orphan. */
int txn_lock(Txn *t, int mode)
{
    if (t->locked) return BANK_E_INVALID;
    sort_ids(t->locks, t->nlocks);
    if (lock_all(t->db, t->locks, t->nlocks, mode) < 0) return BANK_E_IO;
    t->locked = 1;
    t->mode = mode;
    recover_overlapping(t);
    return BANK_OK;
}

/* Read a record we have locked. (Reads the data file, not our staged writes.) */
int txn_read(Txn *t, int file, int index, void *buf)
{
    return db_read(t->db, file, index, buf) < 0 ? BANK_E_IO : BANK_OK;
}

/* Stage a new value for a record we have write-locked.  Nothing is written
 * to disk yet - that happens in txn_commit(). */
int txn_write(Txn *t, int file, int index, const void *buf)
{
    int owned = 0;
    for (int i = 0; i < t->nlocks; i++)
        if (t->locks[i].file == file && t->locks[i].index == index) owned = 1;
    if (!t->locked || t->mode != LK_WRITE || !owned) return BANK_E_INVALID;

    JEntry *e = &t->entry;
    int slot = -1;
    for (int i = 0; i < e->nwrites; i++)         /* same record staged twice? overwrite */
        if (e->writes[i].file == file && e->writes[i].index == index) slot = i;
    if (slot < 0) {
        if (e->nwrites >= MAX_WRITES) return BANK_E_TOO_MANY;
        slot = e->nwrites++;
    }
    e->writes[slot].file  = file;
    e->writes[slot].index = index;
    memset(e->writes[slot].data, 0, MAX_REC);
    memcpy(e->writes[slot].data, buf, db_record_size(file));
    return BANK_OK;
}

/* Unlock everything.  Safe to call more than once. */
void txn_end(Txn *t)
{
    if (t->locked) {
        unlock_all(t->db, t->locks, t->nlocks);
        t->locked = 0;
    }
}

/* Make the staged writes permanent.  THE heart of the project. */
int txn_commit(Txn *t)
{
    Db *db = t->db;
    JEntry *e = &t->entry;
    off_t base = slot_offset(db->my_slot);

    if (!t->locked || t->mode != LK_WRITE) return BANK_E_INVALID;
    if (e->nwrites == 0) { txn_end(t); return BANK_OK; }   /* nothing to do */

    /* STEP 1: write the new values into our journal slot body. */
    e->state = J_EMPTY;                          /* (the real flag lives in the header) */
    if (pwrite(db->journal_fd, e, sizeof *e, base) != (ssize_t)sizeof *e ||
        db_fsync(db->journal_fd) < 0) {
        txn_end(t);
        return BANK_E_IO;                        /* nothing committed: safe */
    }
    maybe_crash(1);          /* test: died before commit -> transaction must vanish */

    /* STEP 2: COMMIT POINT. After the COMMITTED flag is on disk the
     * transaction WILL happen, even if we die right now. */
    publish_committed(db, e);
    if (flush_header(db) < 0) {
        publish_empty(db, db->my_slot);
        txn_end(t);
        return BANK_E_IO;
    }
    maybe_crash(2);          /* test: committed but nothing applied yet */

    /* STEP 3: copy the values into the real data files. */
    if (apply_entry(db, e, 1) < 0) {            /* disk failure: slot stays COMMITTED.   */
        txn_end(t);                             /* The caller should treat this as fatal */
        return BANK_E_IO;                       /* (the server worker exits).            */
    }
    maybe_crash(4);          /* test: all applied, slot still says COMMITTED */

    /* STEP 4: the job is done, clear the flag. */
    publish_empty(db, db->my_slot);

    /* STEP 5: only now let other workers touch these records. */
    txn_end(t);
    return BANK_OK;
}
