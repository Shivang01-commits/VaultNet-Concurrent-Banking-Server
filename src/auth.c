/*
 * auth.c - password hashing with crypt_r() (link with -lcrypt).
 *
 * A stored hash looks like:   $5$aB3dE9xZ$Jx2k....   ($5$ = SHA-256, then the
 * random salt, then the hash).  To check a password we hash it again using the
 * SAME salt (crypt does this when we pass the stored string as the "salt")
 * and compare the two strings.
 */
#include <crypt.h>
#include <string.h>
#include <sys/random.h>

#include "auth.h"

int auth_hash_password(const char *password, char out[PASS_LEN])
{
    static const char alphabet[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[8];
    char setting[16];                      /* "$5$" + 8 salt chars + "$" */
    struct crypt_data data;

    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) return -1;

    memcpy(setting, "$5$", 3);
    for (int i = 0; i < 8; i++) setting[3 + i] = alphabet[rnd[i] % 64];
    setting[11] = '$';
    setting[12] = '\0';

    memset(&data, 0, sizeof data);         /* crypt_r needs a zeroed struct */
    char *h = crypt_r(password, setting, &data);
    if (h == NULL || h[0] == '*' || strlen(h) >= PASS_LEN) return -1;

    strcpy(out, h);
    return 0;
}

int auth_check_password(const char *password, const char *stored)
{
    struct crypt_data data;

    if (stored[0] == '\0') return 0;       /* empty slot: no password */
    memset(&data, 0, sizeof data);
    char *h = crypt_r(password, stored, &data);
    if (h == NULL || h[0] == '*') return 0;
    return strcmp(h, stored) == 0;
}
