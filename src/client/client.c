/* client.c - see client.h */
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "net.h"

int cl_connect(BankConn *c, const char *host, int port)
{
    c->seq = 0;
    c->fd = net_connect(host, port);
    return c->fd < 0 ? -1 : 0;
}

void cl_close(BankConn *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

int cl_send_raw(BankConn *c, const void *buf, size_t n)
{
    return net_write_full(c->fd, buf, n) < 0 ? -1 : 0;
}

int cl_send_packet(BankConn *c, const Packet *p)
{
    return cl_send_raw(c, p, sizeof *p);
}

/* Read exactly one packet, waiting at most timeout_ms for each chunk (<0 = forever). */
int cl_recv_packet(BankConn *c, Packet *reply, int timeout_ms)
{
    size_t got = 0;
    while (got < sizeof *reply) {
        struct pollfd pf = { .fd = c->fd, .events = POLLIN };
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;                       /* timeout or error */
        ssize_t n = read(c->fd, (char *)reply + got, sizeof *reply - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;                       /* closed */
        got += n;
    }
    return 0;
}

int cl_call_timeout(BankConn *c, int op, const void *payload, size_t len, Packet *reply, int timeout_ms)
{
    Packet req, local;

    if (len > PAYLOAD_SIZE) return CL_E_IO;
    if (!reply) reply = &local;
    memset(&req, 0, sizeof req);
    req.magic = PROTO_MAGIC;
    req.op = (uint8_t)op;
    req.seq = ++c->seq;
    if (payload && len) memcpy(req.payload, payload, len);

    if (cl_send_packet(c, &req) < 0) return CL_E_IO;
    if (cl_recv_packet(c, reply, timeout_ms) < 0) return CL_E_IO;
    if (reply->magic != PROTO_MAGIC) return CL_E_IO;
    /* A "server busy" refusal at connect time carries seq 0; everything else must match. */
    if (reply->seq != req.seq && reply->status != PROTO_E_SERVER_BUSY) return CL_E_IO;
    return reply->status;
}

int cl_call(BankConn *c, int op, const void *payload, size_t len, Packet *reply)
{
    return cl_call_timeout(c, op, payload, len, reply, -1);
}

int cl_login(BankConn *c, const char *user, const char *pass, RespLogin *out)
{
    ReqLogin rq;
    Packet rep;

    memset(&rq, 0, sizeof rq);
    snprintf(rq.username, sizeof rq.username, "%s", user);
    snprintf(rq.password, sizeof rq.password, "%s", pass);
    int st = cl_call(c, OP_LOGIN, &rq, sizeof rq, &rep);
    if (st == 0 && out) memcpy(out, rep.payload, sizeof *out);
    return st;
}

int cl_logout(BankConn *c)
{
    return cl_call(c, OP_LOGOUT, NULL, 0, NULL);
}

const char *cl_strerror(int status)
{
    if (status == CL_E_IO) return "connection problem / timeout";
    return proto_strerror(status);
}
