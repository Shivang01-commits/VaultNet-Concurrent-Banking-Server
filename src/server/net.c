/*
 * net.c - socket helpers.  Nothing banking-related in here.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

int net_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int net_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   /* restart without waiting */

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 1024) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int net_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *p;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

/* Non-blocking + send small packets at once + detect silently dead peers. */
void net_tune_client(int fd)
{
    int one = 1, idle = 60, intvl = 10, cnt = 3;
    net_set_nonblock(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
}

/* ---- passing descriptors between processes (SCM_RIGHTS) ---- */

static int send_msg(int sock, char c, int fd)
{
    struct msghdr msg;
    struct iovec iov;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;

    memset(&msg, 0, sizeof msg);
    iov.iov_base = &c;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&ctl, 0, sizeof ctl);
        msg.msg_control = ctl.buf;
        msg.msg_controllen = sizeof ctl.buf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    ssize_t r;
    do { r = sendmsg(sock, &msg, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
    return r == 1 ? 0 : -1;
}

int net_send_fd(int sock, int fd)    { return send_msg(sock, 'F', fd); }
int net_send_byte(int sock, char c)  { return send_msg(sock, c, -1); }

int net_recv_fd(int sock, int *fd, char *byte)
{
    struct msghdr msg;
    struct iovec iov;
    char c;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;

    memset(&msg, 0, sizeof msg);
    iov.iov_base = &c;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof ctl.buf;

    ssize_t r;
    do { r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) return 0;

    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(fd, CMSG_DATA(cm), sizeof(int));
            return 1;
        }
    }
    *byte = c;
    return 2;
}

/* ---- blocking read / write of an exact number of bytes ---- */

int net_read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return got == 0 ? 0 : -1;
        got += r;
    }
    return (int)n;
}

int net_write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t r = send(fd, (const char *)buf + put, n - put, MSG_NOSIGNAL);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        put += r;
    }
    return (int)n;
}

/* ---- housekeeping ---- */

/* A forked child inherits EVERY descriptor of its parent: other clients'
 * sockets, session-lock files, ... A worker must not keep those (a leaked
 * session fd would keep a user "logged in" forever).  So after fork the
 * child closes everything it does not need. */
static int is_kept(const int *keep, int nkeep, int fd)
{
    if (fd < 3) return 1;                         /* stdin/out/err stay */
    for (int k = 0; k < nkeep; k++) if (keep[k] == fd) return 1;
    return 0;
}

void net_close_all_except(const int *keep, int nkeep)
{
    int fds[1024];

    for (;;) {                                    /* repeat until nothing is left to close */
        int n = 0;
        DIR *d = opendir("/proc/self/fd");
        if (!d) break;                            /* no /proc: use the fallback below */
        int dfd = dirfd(d);
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 1024) {
            if (e->d_name[0] == '.') continue;
            int fd = atoi(e->d_name);
            if (fd != dfd && !is_kept(keep, nkeep, fd)) fds[n++] = fd;
        }
        closedir(d);
        if (n == 0) return;
        for (int i = 0; i < n; i++) close(fds[i]);
    }
    /* fallback: brute force up to the limit */
    long max = sysconf(_SC_OPEN_MAX);
    if (max > 1 << 20) max = 1 << 20;
    for (int fd = 3; fd < max; fd++)
        if (!is_kept(keep, nkeep, fd)) close(fd);
}

long net_raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) return 1024;
    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    return (long)rl.rlim_cur;
}
