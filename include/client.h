/*
 * client.h - tiny client library: connect, send one request, get one reply.
 * Used by bank_client (the command-line client) and by the server tests.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include <stddef.h>
#include "proto.h"

#define CL_E_IO  (-200)      /* connection problem (not a server status)  */

typedef struct { int fd; int32_t seq; } BankConn;

int  cl_connect(BankConn *c, const char *host, int port);   /* 0 or -1 */
void cl_close(BankConn *c);

/* Send request `op` with `payload` (len bytes, may be NULL) and wait for the reply.
 * `reply` may be NULL.  Returns the reply status (0 = OK, negative = error)
 * or CL_E_IO.  cl_call_timeout gives up after timeout_ms (CL_E_IO). */
int  cl_call(BankConn *c, int op, const void *payload, size_t len, Packet *reply);
int  cl_call_timeout(BankConn *c, int op, const void *payload, size_t len, Packet *reply, int timeout_ms);

/* Convenience calls */
int  cl_login(BankConn *c, const char *user, const char *pass, RespLogin *out);   /* status */
int  cl_logout(BankConn *c);

/* Low level (tests send broken / pipelined / partial packets with these) */
int  cl_send_raw(BankConn *c, const void *buf, size_t n);                  /* 0 or -1 */
int  cl_send_packet(BankConn *c, const Packet *p);                         /* 0 or -1 */
int  cl_recv_packet(BankConn *c, Packet *reply, int timeout_ms);           /* 0 ok, -1 closed/timeout */

/* Text for a status (BANK_E_*, PROTO_E_* or CL_E_IO) */
const char *cl_strerror(int status);

#endif
