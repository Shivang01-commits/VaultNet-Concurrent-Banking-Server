/*
 * proto.h - the network protocol between client and server.
 *
 * EVERY message (request or reply) is exactly one 512-byte Packet.
 *   - fixed size => the server never needs to "find the end" of a message,
 *     it just waits until 512 bytes have arrived.
 *   - client sends a request, server sends back a reply with the SAME `seq`.
 *   - one request at a time per connection.
 *
 * The payload meaning depends on `op`.  All structs below are plain C structs
 * copied into / out of the payload with memcpy (client and server run on the
 * same kind of machine, so we do not convert byte order).
 *
 * `status` of a reply: 0 = OK, negative = error.
 *     -1 ... -11   BANK_E_* codes from common.h
 *     -100 ...     PROTO_E_* codes below (network-level problems)
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>
#include "common.h"

#define PROTO_MAGIC   0xBA4B
#define PACKET_SIZE   512
#define HEADER_SIZE   16
#define PAYLOAD_SIZE  (PACKET_SIZE - HEADER_SIZE)

#define FLAG_MORE     1        /* reply flag: "there are more pages after this one" */

typedef struct {
    uint16_t magic;            /* must be PROTO_MAGIC                        */
    uint8_t  op;               /* OP_*                                       */
    uint8_t  flags;            /* FLAG_*                                     */
    int32_t  seq;              /* copied from request into reply             */
    int32_t  status;           /* reply only: 0 or negative error            */
    uint16_t count;            /* reply only: number of items in a list page */
    uint16_t reserved;
    uint8_t  payload[PAYLOAD_SIZE];
} Packet;

_Static_assert(sizeof(Packet) == PACKET_SIZE, "Packet must be exactly 512 bytes");

/* ---------------- operations ---------------- */
enum {
    OP_LOGIN = 1, OP_LOGOUT,
    /* customer */
    OP_BALANCE, OP_DEPOSIT, OP_WITHDRAW, OP_TRANSFER, OP_LOAN_APPLY,
    OP_FEEDBACK_ADD, OP_PASSWD, OP_HISTORY, OP_MY_LOANS,
    /* employee */
    OP_ADD_CUSTOMER, OP_MODIFY_USER, OP_ASSIGNED_LOANS, OP_PROCESS_LOAN,
    OP_GET_USER, OP_FIND_USER,
    /* manager */
    OP_SET_ACCOUNT_ACTIVE, OP_ASSIGN_LOAN, OP_PENDING_LOANS, OP_FEEDBACK_LIST,
    /* admin */
    OP_ADD_STAFF, OP_SET_ROLE,
    OP_LAST
};

/* ---------------- network-level status codes ---------------- */
enum {
    PROTO_E_BAD_REQUEST     = -100,  /* unknown op / broken packet            */
    PROTO_E_NOT_LOGGED_IN   = -101,
    PROTO_E_UNKNOWN_RESULT  = -102,  /* the worker died: did it happen? check the passbook */
    PROTO_E_SERVER_BUSY     = -104   /* too many clients / no worker available */
};

const char *proto_strerror(int status);   /* works for BANK_E_* and PROTO_E_* */

/* ---------------- payload layouts ---------------- */

typedef struct { char username[32]; char password[64]; } ReqLogin;
typedef struct { int32_t user_id, role, account_id, pad; } RespLogin;

typedef struct { int64_t amount; } ReqAmount;           /* deposit, withdraw, loan apply */
typedef struct { int64_t balance; } RespBalance;

typedef struct { int32_t to_account; int32_t pad; int64_t amount; } ReqTransfer;

typedef struct { int32_t id, id2; } RespIds;            /* new loan id / new user + account id */

typedef struct { char text[128]; } ReqText;             /* feedback text, new password */

typedef struct {                                        /* add customer / add staff */
    char    username[32];
    char    password[64];
    char    name[32];
    int64_t initial;           /* initial deposit (customers)  */
    int32_t role;              /* staff role (add staff)       */
    int32_t pad;
} ReqAddUser;

typedef struct { int32_t user_id, pad; char name[32]; } ReqModify;

typedef struct { int32_t id, arg; } ReqTwoInts;         /* get user, set active, assign, process, set role */
typedef struct { char username[32]; } ReqFind;

typedef struct {
    int32_t id, role, account_id, active;
    char    username[32];
    char    name[32];
} RespUser;

/* Paged lists.  id = account (history).
 *  after_id = 0 for the first page, then the id of the LAST item you received.
 *  Passbook pages come NEWEST FIRST; the other lists oldest first. */
typedef struct { int32_t id, after_id; } ReqPage;

/* How many list items fit in one reply */
#define PAGE_TXNS      10
#define PAGE_LOANS     12
#define PAGE_FEEDBACK   2

_Static_assert(sizeof(TxnRec)   * PAGE_TXNS     <= PAYLOAD_SIZE, "txn page too big");
_Static_assert(sizeof(Loan)     * PAGE_LOANS    <= PAYLOAD_SIZE, "loan page too big");
_Static_assert(sizeof(Feedback) * PAGE_FEEDBACK <= PAYLOAD_SIZE, "feedback page too big");
_Static_assert(sizeof(ReqAddUser) <= PAYLOAD_SIZE, "ReqAddUser too big");

/* ---------------- small helpers ---------------- */

/* Copy a request payload into a struct (memcpy avoids alignment problems). */
#define PAYLOAD_GET(pkt, var)  memcpy(&(var), (pkt)->payload, sizeof(var))
#define PAYLOAD_PUT(pkt, var)  memcpy((pkt)->payload, &(var), sizeof(var))

#endif /* PROTO_H */
