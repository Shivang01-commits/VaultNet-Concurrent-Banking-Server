/*
 * worker.c - a worker process.
 *
 * A worker is simple on purpose:
 *     loop forever:  read one request from my loop  ->  do it  ->  send the reply
 * It has no clients, no sockets except the one to its loop, and no state
 * between requests.  So if it crashes, nothing is lost except the one
 * request it was working on (and the journal makes that all-or-nothing).
 *
 * It exits when its loop closes the channel (the loop died or is shutting
 * down).  It first finishes the request it is working on, so a request is
 * never cut in half.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "journal.h"
#include "net.h"
#include "server.h"

void worker_main(const ServerConfig *cfg, int chan)
{
    Db db;
    WorkerReq  rq;
    WorkerResp rs;
    int crash_every = 0;

    /* Close everything inherited from the loop except stdin/out/err and our
     * channel: client sockets and session-lock files must NOT stay open
     * here (they would outlive the real owner). */
    net_close_all_except(&chan, 1);

    /* Ctrl-C / kill of the whole group is the supervisor's business.
     * We stop only when the channel closes. */
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);          /* the parent may have blocked signals */
    prctl(PR_SET_PDEATHSIG, SIGKILL);              /* extra safety: die if our loop dies   */

    if (getenv("BANK_NO_FSYNC")) db_sync_enabled = 0;     /* tests only */
    if (getenv("BANK_TEST_CRASH_EVERY")) {                 /* tests only */
        crash_every = atoi(getenv("BANK_TEST_CRASH_EVERY"));
        srand((unsigned)(getpid() * 2654435761u) ^ (unsigned)time(NULL));
    }

    if (db_open(&db, cfg->datadir) < 0) {          /* also runs crash recovery */
        log_msg("worker", "cannot open database in %s", cfg->datadir);
        _exit(2);
    }

    for (;;) {
        ssize_t n;
        do { n = recv(chan, &rq, sizeof rq, 0); } while (n < 0 && errno == EINTR);
        if (n == 0) break;                          /* loop closed the channel: finish */
        if (n != (ssize_t)sizeof rq) break;         /* error or odd size: stop         */

        if (crash_every > 0 && rand() % crash_every == 0)
            journal_crash_point = 1 + rand() % 4;   /* die in the middle of the next commit */

        int login_uid;
        dispatch_request(&db, rq.user_id, &rq.pkt, &rs.pkt, &login_uid);
        rs.client_fd     = rq.client_fd;
        rs.client_gen    = rq.client_gen;
        rs.login_user_id = login_uid;
        rs.pad           = 0;

        if (net_write_full(chan, &rs, sizeof rs) < 0) break;

        /* A disk error while committing leaves our journal slot COMMITTED.
         * Do not carry on with a possibly damaged handle: exit, and the
         * next worker's recovery finishes the job. */
        if (rs.pkt.status == BANK_E_IO) break;
    }

    db_close(&db);
    _exit(0);
}
