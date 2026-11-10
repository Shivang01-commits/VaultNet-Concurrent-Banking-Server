/*
 * db.h - the "database" layer: binary record files + record locking.
 *
 * WHAT THIS LAYER KNOWS
 *   - how to read/write record number N of a file (pread / pwrite)
 *   - how to lock ONE record (not the whole file) with fcntl()
 *
 * WHAT IT DOES NOT KNOW
 *   - anything about banking rules (that is bank_*.c)
 *   - how to make several writes all-or-nothing (that is journal.c)
 *
 * IMPORTANT - one Db handle per worker (process OR thread)
 *   We use "OFD" record locks (F_OFD_SETLK).  They belong to an open file
 *   description, so two threads in the same process that each opened their
 *   own Db will correctly block each other.  (Old-style fcntl locks belong
 *   to the whole process and would NOT block threads of the same process.)
 *   Rule: every worker calls db_open() itself, after fork()/pthread_create().
 */
#ifndef DB_H
#define DB_H

#include <stdint.h>
#include <sys/types.h>
#include "common.h"

/* Journal geometry (used by db.c to open the journal, by journal.c to use it) */
#define JOURNAL_SLOTS 128     /* max number of workers using the DB at once */
#define JSLOT_SIZE    4096    /* bytes reserved for each worker's journal slot  */
#define JHEADER_SIZE  12288   /* journal.dat starts with this many bytes of slot
                               * flags, mapped into memory (see journal.c)      */

/* Lock modes */
#define LK_READ   0         /* many readers may share a record            */
#define LK_WRITE  1         /* only one writer, and no readers at the same time */

typedef struct {
    char dir[256];            /* directory that holds the .dat files        */
    int  fd[F_COUNT];         /* one open file descriptor per data file     */
    int  journal_fd;          /* journal.dat                                */
    int  my_slot;             /* which journal slot belongs to this handle  */
    void *jhead;              /* mmap of the journal header (slot flags)    */
} Db;

/* Set to 0 in stress tests to skip fsync() (much faster). Default 1.
 * Safe for testing process crashes (kill -9) because the OS keeps the
 * page cache; NOT safe against power failure. */
extern int db_sync_enabled;

/* Open (and create if missing) all data files in directory `dir`.
 * Also runs crash recovery and reserves a journal slot.  0 on success. */
int  db_open(Db *db, const char *dir);
void db_close(Db *db);

/* sizeof() of the record stored in `file` */
int  db_record_size(int file);

/* Read / write record number `index` of `file`.
 * db_read gives zeros for records past the end of the file. */
int  db_read (Db *db, int file, int index, void *buf);
int  db_write(Db *db, int file, int index, const void *buf);

/* How many record slots the file currently has (including empty ones) */
int  db_count(Db *db, int file);

/* Force the data of one file to disk (no-op if db_sync_enabled == 0) */
int  db_fsync(int fd);

/* Lock / unlock one record.  mode = LK_READ or LK_WRITE.
 * wait = 1: sleep until we get it.   wait = 0: return -1 at once if busy. */
int  db_lock_record  (Db *db, int file, int index, int mode, int wait);
int  db_unlock_record(Db *db, int file, int index);

/* Same thing for a raw byte range of any fd (used for sessions & journal) */
int  db_lock_range  (int fd, off_t start, off_t len, int mode, int wait);
int  db_unlock_range(int fd, off_t start, off_t len);

/* Hand out a fresh unique id for `file` (users, accounts, ...).
 * Ids start at 1.  Takes the lock on the counter record for a moment only. */
int64_t db_alloc_id(Db *db, int file);

#endif /* DB_H */
