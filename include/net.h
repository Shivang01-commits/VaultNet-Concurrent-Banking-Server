/*
 * net.h - small socket helpers shared by server and client.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>

int  net_set_nonblock(int fd);
int  net_listen(int port);                       /* non-blocking listening socket, or -1 */
int  net_connect(const char *host, int port);    /* blocking connected socket, or -1     */
void net_tune_client(int fd);                    /* non-blocking, NODELAY, keepalive     */

/* Pass an open file descriptor to another process over an AF_UNIX socket. */
int  net_send_fd(int sock, int fd);
int  net_send_byte(int sock, char c);            /* plain 1-byte message, no fd          */
/* Receive: returns 1 = got a descriptor (*fd), 2 = got a plain byte (*byte),
 *          0 = peer closed, -1 = error (errno EAGAIN when nothing to read). */
int  net_recv_fd(int sock, int *fd, char *byte);

/* Blocking read / write of exactly n bytes (handles EINTR and short counts).
 * read returns n, 0 on EOF before any byte, -1 on error or EOF in the middle. */
int  net_read_full(int fd, void *buf, size_t n);
int  net_write_full(int fd, const void *buf, size_t n);

/* Close every open descriptor except the ones in keep[0..nkeep). */
void net_close_all_except(const int *keep, int nkeep);

/* Raise the soft open-files limit to the hard limit; returns the new limit. */
long net_raise_fd_limit(void);

#endif
