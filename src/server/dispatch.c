/*
 * dispatch.c - turns one request Packet into a call of the banking API
 * and builds the reply Packet.  No sockets here, no epoll: just
 *      Packet in  ->  bank_*()  ->  Packet out.
 */
#include <string.h>

#include "server.h"

/* Copy a text field of a request into a local buffer and make sure it ends
 * with '\0' (a hostile client may send 32 bytes without one). */
static void get_str(char *dst, size_t dstsize, const char *src, size_t srcsize)
{
    size_t n = srcsize < dstsize - 1 ? srcsize : dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void fill_user_reply(Packet *rep, const User *u)
{
    RespUser r;
    memset(&r, 0, sizeof r);
    r.id = u->id;
    r.role = u->role;
    r.account_id = u->account_id;
    r.active = u->active;
    memcpy(r.username, u->username, sizeof r.username);
    memcpy(r.name, u->name, sizeof r.name);
    PAYLOAD_PUT(rep, r);
}

int dispatch_request(Db *db, int user_id, const Packet *req, Packet *rep, int *login_user_id)
{
    User me, other;
    int rc = BANK_OK;

    *login_user_id = -1;
    memset(rep, 0, sizeof *rep);                 /* never leak old bytes to a client */
    rep->magic = PROTO_MAGIC;
    rep->op    = req->op;
    rep->seq   = req->seq;

    /* ---- LOGIN is the only thing allowed without a session ---- */
    if (req->op == OP_LOGIN) {
        ReqLogin in;
        char name[33], pass[65];
        PAYLOAD_GET(req, in);
        get_str(name, sizeof name, in.username, sizeof in.username);
        get_str(pass, sizeof pass, in.password, sizeof in.password);

        rc = bank_authenticate(db, name, pass, &me);   /* does NOT take the session lock: the loop does */
        if (rc == BANK_OK) {
            RespLogin out = { me.id, me.role, me.account_id, 0 };
            PAYLOAD_PUT(rep, out);
            *login_user_id = me.id;
        }
        rep->status = rc;
        return rc;
    }

    if (user_id <= 0) {
        rep->status = PROTO_E_NOT_LOGGED_IN;
        return rep->status;
    }

    /* Read the user again for EVERY request: if an admin deactivated this
     * user or changed the role a second ago, it applies right now. */
    rc = bank_refresh_user(db, user_id, &me);
    if (rc != BANK_OK) { rep->status = rc; return rc; }

    switch (req->op) {

    case OP_LOGOUT:                      /* normally handled by the loop; harmless here */
        break;

    /* ---------------- customer ---------------- */
    case OP_BALANCE: {
        RespBalance out;
        rc = bank_balance(db, &me, &out.balance);
        if (rc == BANK_OK) PAYLOAD_PUT(rep, out);
        break;
    }
    case OP_DEPOSIT:
    case OP_WITHDRAW: {
        ReqAmount in;
        RespBalance out;
        PAYLOAD_GET(req, in);
        rc = (req->op == OP_DEPOSIT) ? bank_deposit(db, &me, in.amount, &out.balance)
                                     : bank_withdraw(db, &me, in.amount, &out.balance);
        if (rc == BANK_OK) PAYLOAD_PUT(rep, out);
        break;
    }
    case OP_TRANSFER: {
        ReqTransfer in;
        RespBalance out;
        PAYLOAD_GET(req, in);
        rc = bank_transfer(db, &me, in.to_account, in.amount, &out.balance);
        if (rc == BANK_OK) PAYLOAD_PUT(rep, out);
        break;
    }
    case OP_LOAN_APPLY: {
        ReqAmount in;
        RespIds out = { 0, 0 };
        PAYLOAD_GET(req, in);
        rc = bank_apply_loan(db, &me, in.amount, &out.id);
        if (rc == BANK_OK) PAYLOAD_PUT(rep, out);
        break;
    }
    case OP_FEEDBACK_ADD: {
        ReqText in;
        char text[TEXT_LEN + 1];
        PAYLOAD_GET(req, in);
        get_str(text, sizeof text, in.text, sizeof in.text);
        rc = bank_add_feedback(db, &me, text);
        break;
    }
    case OP_PASSWD: {
        ReqText in;
        char text[TEXT_LEN + 1];
        PAYLOAD_GET(req, in);
        get_str(text, sizeof text, in.text, sizeof in.text);
        rc = bank_change_password(db, &me, text);
        break;
    }

    /* ---------------- paged lists ---------------- */
    case OP_HISTORY: {
        ReqPage in;
        TxnRec items[PAGE_TXNS];
        int count = 0, more = 0;
        PAYLOAD_GET(req, in);
        if (in.id == 0) in.id = me.account_id;          /* customers: "my own account" */
        rc = bank_history_page(db, &me, in.id, in.after_id, items, PAGE_TXNS, &count, &more);
        if (rc == BANK_OK) {
            memcpy(rep->payload, items, sizeof(TxnRec) * count);
            rep->count = (uint16_t)count;
            if (more) rep->flags |= FLAG_MORE;
        }
        break;
    }
    case OP_MY_LOANS:
    case OP_ASSIGNED_LOANS:
    case OP_PENDING_LOANS: {
        ReqPage in;
        Loan items[PAGE_LOANS];
        int count = 0, more = 0;
        PAYLOAD_GET(req, in);
        if (req->op == OP_MY_LOANS)
            rc = bank_my_loans_page(db, &me, in.after_id, items, PAGE_LOANS, &count, &more);
        else if (req->op == OP_ASSIGNED_LOANS)
            rc = bank_assigned_loans_page(db, &me, in.after_id, items, PAGE_LOANS, &count, &more);
        else
            rc = bank_pending_loans_page(db, &me, in.after_id, items, PAGE_LOANS, &count, &more);
        if (rc == BANK_OK) {
            memcpy(rep->payload, items, sizeof(Loan) * count);
            rep->count = (uint16_t)count;
            if (more) rep->flags |= FLAG_MORE;
        }
        break;
    }
    case OP_FEEDBACK_LIST: {
        ReqPage in;
        Feedback items[PAGE_FEEDBACK];
        int count = 0, more = 0;
        PAYLOAD_GET(req, in);
        rc = bank_feedback_page(db, &me, in.after_id, items, PAGE_FEEDBACK, &count, &more);
        if (rc == BANK_OK) {
            memcpy(rep->payload, items, sizeof(Feedback) * count);
            rep->count = (uint16_t)count;
            if (more) rep->flags |= FLAG_MORE;
        }
        break;
    }

    /* ---------------- employee ---------------- */
    case OP_ADD_CUSTOMER: {
        ReqAddUser in;
        char uname[33], pass[65], name[33];
        RespIds out = { 0, 0 };
        PAYLOAD_GET(req, in);
        get_str(uname, sizeof uname, in.username, sizeof in.username);
        get_str(pass,  sizeof pass,  in.password, sizeof in.password);
        get_str(name,  sizeof name,  in.name,     sizeof in.name);
        rc = bank_add_customer(db, &me, uname, pass, name, in.initial, &out.id);
        if (rc == BANK_OK) {
            if (bank_get_user(db, &me, out.id, &other) == BANK_OK) out.id2 = other.account_id;
            PAYLOAD_PUT(rep, out);
        }
        break;
    }
    case OP_MODIFY_USER: {
        ReqModify in;
        char name[33];
        PAYLOAD_GET(req, in);
        get_str(name, sizeof name, in.name, sizeof in.name);
        rc = bank_modify_user(db, &me, in.user_id, name);
        break;
    }
    case OP_PROCESS_LOAN: {
        ReqTwoInts in;
        PAYLOAD_GET(req, in);
        rc = bank_process_loan(db, &me, in.id, in.arg);
        break;
    }
    case OP_GET_USER: {
        ReqTwoInts in;
        PAYLOAD_GET(req, in);
        rc = bank_get_user(db, &me, in.id, &other);
        if (rc == BANK_OK) fill_user_reply(rep, &other);
        break;
    }
    case OP_FIND_USER: {
        ReqFind in;
        char uname[33];
        PAYLOAD_GET(req, in);
        get_str(uname, sizeof uname, in.username, sizeof in.username);
        rc = bank_find_user(db, &me, uname, &other);
        if (rc == BANK_OK) fill_user_reply(rep, &other);
        break;
    }

    /* ---------------- manager ---------------- */
    case OP_SET_ACCOUNT_ACTIVE: {
        ReqTwoInts in;
        PAYLOAD_GET(req, in);
        rc = bank_set_account_active(db, &me, in.id, in.arg);
        break;
    }
    case OP_ASSIGN_LOAN: {
        ReqTwoInts in;
        PAYLOAD_GET(req, in);
        rc = bank_assign_loan(db, &me, in.id, in.arg);
        break;
    }

    /* ---------------- admin ---------------- */
    case OP_ADD_STAFF: {
        ReqAddUser in;
        char uname[33], pass[65], name[33];
        RespIds out = { 0, 0 };
        PAYLOAD_GET(req, in);
        get_str(uname, sizeof uname, in.username, sizeof in.username);
        get_str(pass,  sizeof pass,  in.password, sizeof in.password);
        get_str(name,  sizeof name,  in.name,     sizeof in.name);
        rc = bank_add_employee(db, &me, uname, pass, name, in.role, &out.id);
        if (rc == BANK_OK) PAYLOAD_PUT(rep, out);
        break;
    }
    case OP_SET_ROLE: {
        ReqTwoInts in;
        PAYLOAD_GET(req, in);
        rc = bank_set_role(db, &me, in.id, in.arg);
        break;
    }

    default:
        rc = PROTO_E_BAD_REQUEST;
        break;
    }

    rep->status = rc;
    return rc;
}
