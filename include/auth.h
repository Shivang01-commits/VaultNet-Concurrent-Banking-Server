/*
 * auth.h - password hashing.  We never store a password, only its
 * salted SHA-256 hash (made by the system's crypt_r()).
 */
#ifndef AUTH_H
#define AUTH_H

#include "common.h"

/* Hash `password` with a fresh random salt. `out` receives a text string. 0 = ok */
int auth_hash_password(const char *password, char out[PASS_LEN]);

/* 1 if `password` matches the stored hash, else 0 */
int auth_check_password(const char *password, const char *stored);

#endif
