/* log.c - tiny logger: "HH:MM:SS [pid who] message" on stderr. */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "server.h"

int log_verbose = 0;

static void vlog(const char *who, const char *fmt, va_list ap)
{
    char line[512];
    char tbuf[16];
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tmv);
    int n = snprintf(line, sizeof line, "%s [%d %s] ", tbuf, (int)getpid(), who);
    if (n < 0 || n >= (int)sizeof line) return;
    int m = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    if (m < 0) return;
    n += m;
    if (n >= (int)sizeof line - 1) n = sizeof line - 2;
    line[n++] = '\n';
    if (write(2, line, n) < 0) { /* nothing sensible to do */ }   /* one write = lines never mix */
}

void log_msg(const char *who, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(who, fmt, ap);
    va_end(ap);
}

void log_debug(const char *who, const char *fmt, ...)
{
    if (!log_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(who, fmt, ap);
    va_end(ap);
}
