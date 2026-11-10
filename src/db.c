/*
 * db.c - binary record files and record-level locking.
 * See db.h for the big picture.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE            /* needed for F_OFD_SETLK */
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "journal.h"

int db_sync_enabled = 1;

/* File names, in the same order as the F_* ids in common.h */
static const char *FILE_NAMES[F_COUNT] = {
    "users.dat", "accounts.dat", "txns.dat", "loans.dat", "feedback.dat", "meta.dat", "names.dat"
};

/* ------------------------------------------------------------------ */
/* Record sizes                                                        */
/* ------------------------------------------------------------------ */
int db_record_size(int file)
{
    switch (file) {
    case F_USERS:    return sizeof(User);
    case F_ACCOUNTS: return sizeof(Account);
    case F_TXNS:     return sizeof(TxnRec);
    case F_LOANS:    return sizeof(Loan);
    case F_FEEDBACK: return sizeof(Feedback);
    case F_META:     return sizeof(Meta);
    case F_NAMES:    return sizeof(NameSlot);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reading and writing records                                         */
/* ------------------------------------------------------------------ */

/* Read record `index`.  A record that is not on disk yet reads as all zeros
 * (which means "empty slot", because id 0 is never used). */
int db_read(Db *db, int file, int index, void *buf)
{
    int size = db_record_size(file);
    off_t off = (off_t)index * size;
    int done = 0;

    while (done < size) {
        ssize_t n = pread(db->fd[file], (char *)buf + done, size - done, off + done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;              /* reached end of file */
        done += (int)n;
    }
    if (done < size)                     /* past the end: fill with zeros */
        memset((char *)buf + done, 0, size - done);
    return 0;
}

/* Write record `index`.  Writing past the end of the file is fine: the
 * gap is filled with zeros by the OS. */
int db_write(Db *db, int file, int index, const void *buf)
{
    int size = db_record_size(file);
    off_t off = (off_t)index * size;
    int done = 0;

    while (done < size) {
        ssize_t n = pwrite(db->fd[file], (const char *)buf + done, size - done, off + done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (int)n;
    }
    return 0;
}

int db_count(Db *db, int file)
{
    struct stat st;
    if (fstat(db->fd[file], &st) < 0) return 0;
    return (int)(st.st_size / db_record_size(file));
}

int db_fsync(int fd)
{
    if (!db_sync_enabled) return 0;
    return fsync(fd);
}

/* ------------------------------------------------------------------ */
/* Record locking                                                      */
/* ------------------------------------------------------------------ */

/* Lock bytes [start, start+len) of fd.
 *   F_OFD_SETLKW = lock, and SLEEP if somebody else holds it
 *   F_OFD_SETLK  = lock, or fail at once with errno EAGAIN
 * The kernel releases every lock automatically if the process dies -
 * this is what makes crash recovery possible. */
int db_lock_range(int fd, off_t start, off_t len, int mode, int wait)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);          /* l_pid MUST be 0 for OFD locks */
    fl.l_type   = (mode == LK_WRITE) ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = start;
    fl.l_len    = len;

    int cmd = wait ? F_OFD_SETLKW : F_OFD_SETLK;
    while (fcntl(fd, cmd, &fl) < 0) {
        if (errno == EINTR) continue;   /* interrupted by a signal: retry */
        return -1;                      /* busy (EAGAIN) or real error */
    }
    return 0;
}

int db_unlock_range(int fd, off_t start, off_t len)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = start;
    fl.l_len    = len;
    return fcntl(fd, F_OFD_SETLK, &fl);
}

/* Lock exactly the bytes of record `index`. */
int db_lock_record(Db *db, int file, int index, int mode, int wait)
{
    int size = db_record_size(file);
    return db_lock_range(db->fd[file], (off_t)index * size, size, mode, wait);
}

int db_unlock_record(Db *db, int file, int index)
{
    int size = db_record_size(file);
    return db_unlock_range(db->fd[file], (off_t)index * size, size);
}

/* ------------------------------------------------------------------ */
/* Unique id counters (meta.dat record 0)                              */
/* ------------------------------------------------------------------ */

/* Give out the next id for `file`.
 *
 * The counter is NOT part of a journal transaction on purpose: the lock is
 * held for a few microseconds only, so it never slows other requests.
 * If a worker dies after taking an id but before using it, that id is
 * simply never used (a harmless gap).  Ids are never handed out twice.   */
int64_t db_alloc_id(Db *db, int file)
{
    Meta m;
    int64_t id;

    if (db_lock_record(db, F_META, META_COUNTERS, LK_WRITE, 1) < 0) return -1;
    db_read(db, F_META, META_COUNTERS, &m);
    id = ++m.next_id[file];
    db_write(db, F_META, META_COUNTERS, &m);
    db_unlock_record(db, F_META, META_COUNTERS);
    return id;
}

/* After a power failure the counters (not fsync'ed, for speed) could be
 * behind the real data.  At open time, raise each counter so it is at
 * least the highest record that really exists in the file. */
static void repair_counters(Db *db)
{
    Meta m;
    int changed = 0;

    db_lock_record(db, F_META, META_COUNTERS, LK_WRITE, 1);
    db_read(db, F_META, META_COUNTERS, &m);
    for (int f = 0; f < F_META; f++) {
        int n = db_count(db, f);
        if (n > 0 && m.next_id[f] < n - 1) {
            m.next_id[f] = n - 1;
            changed = 1;
        }
    }
    if (changed) db_write(db, F_META, META_COUNTERS, &m);
    db_unlock_record(db, F_META, META_COUNTERS);
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */
int db_open(Db *db, const char *dir)
{
    char path[512];

    memset(db, 0, sizeof *db);
    for (int i = 0; i < F_COUNT; i++) db->fd[i] = -1;
    db->journal_fd = -1;
    db->my_slot = -1;
    snprintf(db->dir, sizeof db->dir, "%s", dir);

    if (mkdir(dir, 0755) < 0 && errno != EEXIST) return -1;

    for (int i = 0; i < F_COUNT; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, FILE_NAMES[i]);
        db->fd[i] = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (db->fd[i] < 0) { db_close(db); return -1; }
    }
    snprintf(path, sizeof path, "%s/journal.dat", dir);
    db->journal_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (db->journal_fd < 0) { db_close(db); return -1; }

    /* The first JHEADER_SIZE bytes of journal.dat hold the "slot flags".  We map
     * them into memory so that checking all slots costs no system calls.
     * (Every process maps the SAME file, so all of them see the same memory.) */
    struct stat st;
    if (fstat(db->journal_fd, &st) < 0) { db_close(db); return -1; }
    if (st.st_size < JHEADER_SIZE && ftruncate(db->journal_fd, JHEADER_SIZE) < 0) {
        db_close(db);
        return -1;
    }
    db->jhead = mmap(NULL, JHEADER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, db->journal_fd, 0);
    if (db->jhead == MAP_FAILED) { db->jhead = NULL; db_close(db); return -1; }

    /* 1. Finish any transaction that a crashed worker left half-done. */
    journal_recover_all(db);
    /* 2. Refuse data written by an older version (it has no username index). */
    if (db_count(db, F_USERS) > 1 && db_count(db, F_NAMES) == 0) {
        fprintf(stderr, "db_open: %s was created by an older version of this program "
                        "(no names.dat index). Delete the folder and start again.\n", dir);
        db_close(db);
        return -1;
    }
    /* 3. Make sure id counters are not behind the data. */
    repair_counters(db);
    /* 4. Reserve a journal slot for this handle. */
    if (journal_claim_slot(db) < 0) { db_close(db); return -1; }
    return 0;
}

void db_close(Db *db)
{
    /* Closing the fds also releases every lock this handle still holds. */
    for (int i = 0; i < F_COUNT; i++)
        if (db->fd[i] >= 0) close(db->fd[i]);
    if (db->jhead) munmap(db->jhead, JHEADER_SIZE);
    if (db->journal_fd >= 0) close(db->journal_fd);
    db->jhead = NULL;
    for (int i = 0; i < F_COUNT; i++) db->fd[i] = -1;
    db->journal_fd = -1;
    db->my_slot = -1;
}
