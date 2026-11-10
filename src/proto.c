/* proto.c - text for status codes (used by server logs and the client). */
#include "proto.h"

const char *bank_strerror(int code);

const char *proto_strerror(int status)
{
    switch (status) {
    case PROTO_E_BAD_REQUEST:    return "bad request";
    case PROTO_E_NOT_LOGGED_IN:  return "not logged in";
    case PROTO_E_UNKNOWN_RESULT: return "server worker failed - result UNKNOWN, check your passbook before retrying";
    case PROTO_E_SERVER_BUSY:    return "server busy, try again";
    }
    return bank_strerror(status);
}
