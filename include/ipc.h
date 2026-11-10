/*
 * ipc.h - what an event-loop process and its worker processes send each other.
 *
 * They talk over a socketpair(AF_UNIX, SOCK_SEQPACKET): every send() is
 * exactly one message and arrives whole, so no "partial read" handling.
 *
 * The worker never sees the client socket.  The loop gives it a request,
 * the worker returns the finished 512-byte reply, the loop sends it.
 */
#ifndef IPC_H
#define IPC_H

#include "proto.h"

typedef struct {
    int32_t  client_fd;        /* which client (echoed back, worker ignores it) */
    uint32_t client_gen;       /* "generation" of that fd (see loop.c)          */
    int32_t  user_id;          /* logged-in user of that client, 0 = nobody     */
    int32_t  pad;
    Packet   pkt;              /* the request                                   */
} WorkerReq;

typedef struct {
    int32_t  client_fd;        /* copied from the request                       */
    uint32_t client_gen;
    int32_t  login_user_id;    /* >0 only for a successful LOGIN (loop then takes the session lock) */
    int32_t  pad;
    Packet   pkt;              /* the finished reply                            */
} WorkerResp;

#endif
