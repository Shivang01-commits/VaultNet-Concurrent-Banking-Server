/*
 * server.h - the three kinds of server process.
 *
 *   supervisor : accepts clients, hands each one to the least-busy loop,
 *                restarts anything that dies.
 *   loop       : one epoll event loop.  Owns many client sockets, reads
 *                requests, gives them to workers, sends back replies.
 *   worker     : does the real banking work (calls bank_*), one request at a time.
 *
 *   supervisor -- starts --> N loops -- each starts --> W workers
 */
#ifndef SERVER_H
#define SERVER_H

#include "bank.h"
#include "proto.h"

typedef struct {
    int         port;                 /* TCP port to listen on                 */
    const char *datadir;              /* directory with the .dat files         */
    int         nloops;               /* number of event-loop processes        */
    int         nworkers;             /* workers per loop                      */
    int         idle_secs;            /* disconnect clients silent this long   */
    int         worker_timeout_secs;  /* kill a worker stuck on one request    */
    int         max_clients_per_loop; /* 0 = as many as the fd limit allows    */
    int         verbose;              /* 1 = log connects / logins             */
} ServerConfig;

/* The three process mains.  None of them returns. */
void supervisor_main(const ServerConfig *cfg);
void loop_main(const ServerConfig *cfg, int ctrl_fd);   /* ctrl_fd: link to supervisor */
void worker_main(const ServerConfig *cfg, int chan_fd); /* chan_fd: link to its loop   */

/* Run one request.  Used by the worker (and could be called directly in tests).
 *   user_id          : who is logged in on that connection (0 = nobody)
 *   login_user_id    : set to the user id after a successful LOGIN, else -1
 * Fills *rep and returns rep->status. */
int dispatch_request(Db *db, int user_id, const Packet *req, Packet *rep, int *login_user_id);

/* logging (to stderr, with time and process id) */
extern int log_verbose;
void log_msg(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_debug(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
