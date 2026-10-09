/* include/kernel/socket.h -- AF_UNIX stream sockets (fs/unixsock.c)
 *
 * What X11 needs locally: SOCK_STREAM sockets named either by a path in the
 * file system ("/tmp/.X11-unix/X0", a VSOCK node) or in Linux's abstract
 * namespace (sun_path[0] == 0, "@/tmp/.X11-unix/X0", which xtrans tries
 * first). connect() completes at once by queueing a server-side socket on
 * the listener; accept() hands it out. Each direction has a 64 KiB buffer;
 * blocking, O_NONBLOCK / MSG_DONTWAIT, EOF and poll() follow POSIX/Linux.
 * Peer credentials (SO_PEERCRED) are the creating process' pid/uid/gid.
 * Not yet: SCM_RIGHTS descriptor passing, SOCK_DGRAM/SEQPACKET. */
#ifndef KESTREL_SOCKET_H
#define KESTREL_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000
#define SOL_SOCKET      1
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_PEERCRED     17
#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2
#define UNIX_PATH_MAX   108

struct sockaddr_un { uint16_t sun_family; char sun_path[UNIX_PATH_MAX]; };
struct ucred { int32_t pid; uint32_t uid, gid; };

struct file;

bool  usock_is(struct file *f);
int   usock_create(int domain, int type, int protocol, struct file **out);
int   usock_pair(int type, struct file **a, struct file **b);
int   usock_bind(struct file *f, const struct sockaddr_un *a, uint32_t len);
int   usock_listen(struct file *f, int backlog);
int   usock_connect(struct file *f, const struct sockaddr_un *a, uint32_t len);
int   usock_accept(struct file *f, int flags, struct file **out, struct sockaddr_un *peer, uint32_t *len);
long  usock_send(struct file *f, const void *buf, size_t len, int flags);
long  usock_recv(struct file *f, void *buf, size_t len, int flags);
int   usock_shutdown(struct file *f, int how);
int   usock_name(struct file *f, bool peer, struct sockaddr_un *a, uint32_t *len);
int   usock_getsockopt(struct file *f, int level, int opt, void *val, uint32_t *len);

#endif
