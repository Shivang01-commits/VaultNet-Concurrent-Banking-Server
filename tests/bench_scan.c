/*
 * bench_scan.c - how much does every request pay for the "is there a dead
 * worker's committed transaction on my records?" check?
 *
 * Build it against two source trees (old = journal flags read with pread from
 * the file, new = flags in a memory-mapped header) and compare:
 *     gcc -O2 -D_GNU_SOURCE -I<tree>/include tests/bench_scan.c <tree>/src/{db,journal,auth,bank_*}.c -lcrypt -pthread
 * Uses read-only passbook calls so the number shows the check, not the disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "bank.h"

int main(int argc, char **argv)
{
    char dir[] = "/tmp/bench_scan_XXXXXX";
    Db db;
    User admin, emp, c;
    TxnRec lines[4];
    int id, count, n = argc > 1 ? atoi(argv[1]) : 200000;

    if (!mkdtemp(dir) || db_open(&db, dir) < 0) return 2;
    db_sync_enabled = 0;
    bank_create_first_admin(&db, "admin", "pw");
    bank_authenticate(&db, "admin", "pw", &admin);
    bank_add_employee(&db, &admin, "emp", "pw", "E", ROLE_EMPLOYEE, &id);
    bank_authenticate(&db, "emp", "pw", &emp);
    bank_add_customer(&db, &emp, "c", "pw", "C", 100, &id);
    bank_authenticate(&db, "c", "pw", &c);

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++) bank_history(&db, &c, c.account_id, lines, 4, &count);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1000.0 / n;
    printf("JOURNAL_SLOTS=%d: %.2f microseconds per read-only request\n", JOURNAL_SLOTS, us);

    db_close(&db);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) {}
    return 0;
}
