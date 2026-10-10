/* user/socktest.c -- AF_UNIX stream sockets on Kestrel, the way X11 uses
 * them: a listening socket named both in the file system
 * (/tmp/.X11-unix/X9) and in the abstract namespace, non-blocking accept
 * driven by poll(), SO_PEERCRED, short reads/writes of a large transfer,
 * EOF and EPIPE on close.
 *
 *   socktest           everything inside one process
 *   socktest server    serve one client, then exit   } run both at once:
 *   socktest client    connect, exchange, exit       } kestrel.exec=...+...
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE                     /* struct ucred */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

static int failures;
static const char *who = "socktest";
#define CHECK(c, what) do { int ok_ = (c); printf("%s:  %s  %s\n", who, ok_ ? "ok  " : "FAIL", what); if (!ok_) { failures++; } } while (0)

#define SOCK_DIR  "/tmp/.X11-unix"
#define SOCK_PATH "/tmp/.X11-unix/X9"
#define ABSTRACT  "/tmp/.X11-unix/X9"           /* xtrans' abstract name: "\\0" + path */

static socklen_t abstract_addr(struct sockaddr_un *a)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    a->sun_path[0] = 0;
    memcpy(a->sun_path + 1, ABSTRACT, strlen(ABSTRACT));
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(ABSTRACT));
}

static socklen_t path_addr(struct sockaddr_un *a)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, SOCK_PATH);
    return (socklen_t)sizeof *a;
}

/* Like the X server: both names, non-blocking, close-on-exec. */
static int make_listeners(int *lp, int *la)
{
    struct sockaddr_un a;
    mkdir(SOCK_DIR, 01777);
    unlink(SOCK_PATH);
    *lp = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    *la = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (*lp < 0 || *la < 0) return -1;
    if (bind(*lp, (struct sockaddr *)&a, path_addr(&a)) < 0) return -2;
    if (bind(*la, (struct sockaddr *)&a, abstract_addr(&a)) < 0) return -3;
    if (listen(*lp, 5) < 0 || listen(*la, 5) < 0) return -4;
    return 0;
}

static int connect_to(int abstract)
{
    struct sockaddr_un a;
    socklen_t n = abstract ? abstract_addr(&a) : path_addr(&a);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, n) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EAGAIN) { struct pollfd q = { fd, POLLOUT, 0 }; poll(&q, 1, 1000); continue; }
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EAGAIN) { struct pollfd q = { fd, POLLIN, 0 }; poll(&q, 1, 1000); continue; }
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

#define BIG (300 * 1024)                        /* bigger than the 64 KiB socket buffer */

static unsigned char pattern(size_t i) { return (unsigned char)(i * 7 + (i >> 9)); }

static int selftest(void)
{
    int lp, la, rc = make_listeners(&lp, &la);
    CHECK(rc == 0, "socket/bind/listen on a path and an abstract name");
    if (rc) return 1;
    struct stat st;
    CHECK(stat(SOCK_PATH, &st) == 0 && S_ISSOCK(st.st_mode), "the path name is a socket in the file system");
    struct sockaddr_un a;
    errno = 0;
    CHECK(bind(socket(AF_UNIX, SOCK_STREAM, 0), (struct sockaddr *)&a, abstract_addr(&a)) < 0 && errno == EADDRINUSE,
          "binding the same abstract name again: EADDRINUSE");
    errno = 0;
    CHECK(accept(lp, NULL, NULL) < 0 && errno == EAGAIN, "non-blocking accept with nobody waiting: EAGAIN");

    int c1 = connect_to(0), c2 = connect_to(1);
    CHECK(c1 >= 0, "connect by path");
    CHECK(c2 >= 0, "connect by abstract name");
    struct pollfd q[2] = { { lp, POLLIN, 0 }, { la, POLLIN, 0 } };
    CHECK(poll(q, 2, 1000) == 2, "poll: both listeners readable");
    int s1 = accept4(lp, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC), s2 = accept(la, NULL, NULL);
    CHECK(s1 >= 0 && s2 >= 0, "accept on both");

    struct ucred cr; socklen_t cl = sizeof cr;
    CHECK(getsockopt(s1, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid == getpid() && cl == sizeof cr,
          "SO_PEERCRED names the client process");
    socklen_t al = sizeof a;
    CHECK(getpeername(c1, (struct sockaddr *)&a, &al) == 0 && !strcmp(a.sun_path, SOCK_PATH), "getpeername of the client side");

    CHECK(write(c1, "hello", 5) == 5, "client writes");
    char b[16] = { 0 };
    CHECK(read(s1, b, sizeof b) == 5 && !memcmp(b, "hello", 5), "server reads it");
    CHECK(send(s2, "abc", 3, 0) == 3 && recv(c2, b, sizeof b, 0) == 3 && !memcmp(b, "abc", 3), "send/recv the other way");
    errno = 0;
    CHECK(recv(c2, b, sizeof b, MSG_DONTWAIT) < 0 && errno == EAGAIN, "MSG_DONTWAIT on empty socket: EAGAIN");

    /* a transfer larger than the buffer: the writer must stop with EAGAIN, then resume */
    unsigned char *big = malloc(BIG), *got = malloc(BIG);
    for (size_t i = 0; i < BIG; i++) big[i] = pattern(i);
    size_t sent = 0, recvd = 0;
    int spins = 0;
    while (recvd < BIG && spins++ < 10000) {
        if (sent < BIG) { ssize_t n = write(s1, big + sent, BIG - sent); if (n > 0) sent += (size_t)n; }
        ssize_t n = recv(c1, got + recvd, BIG - recvd, MSG_DONTWAIT);
        if (n > 0) recvd += (size_t)n;
    }
    CHECK(recvd == BIG && !memcmp(big, got, BIG), "300 KiB through a 64 KiB socket, interleaved non-blocking I/O");

    close(c1);
    q[0].fd = s1; q[0].events = POLLIN;
    CHECK(poll(q, 1, 0) == 1 && (q[0].revents & POLLHUP), "peer closed: POLLHUP");
    CHECK(read(s1, b, sizeof b) == 0, "peer closed: read returns 0 (EOF)");
    errno = 0;
    CHECK(send(s1, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "peer closed: send(MSG_NOSIGNAL) fails with EPIPE");
    signal(SIGPIPE, SIG_IGN);                   /* as X servers and clients do */
    errno = 0;
    CHECK(write(s1, "x", 1) < 0 && errno == EPIPE, "peer closed: write fails with EPIPE (SIGPIPE ignored)");
    signal(SIGPIPE, SIG_DFL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && write(sv[0], "pq", 2) == 2 && read(sv[1], b, 2) == 2 && b[1] == 'q',
          "socketpair");
    unlink(SOCK_PATH);
    errno = 0;
    CHECK(connect_to(0) < 0 && errno == ENOENT, "after unlink the path name is gone");
    return failures != 0;
}

static int server(void)
{
    who = "server";
    int lp, la;
    CHECK(make_listeners(&lp, &la) == 0, "listening on " SOCK_PATH " and @" ABSTRACT);
    struct pollfd q[2] = { { lp, POLLIN, 0 }, { la, POLLIN, 0 } };
    CHECK(poll(q, 2, 10000) > 0, "a client arrives (poll)");
    int s = accept4((q[0].revents & POLLIN) ? lp : la, NULL, NULL, SOCK_NONBLOCK);
    CHECK(s >= 0, "accept");
    struct ucred cr; socklen_t cl = sizeof cr;
    CHECK(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid != getpid(), "SO_PEERCRED: the client is another process");
    printf("server: client is pid %d\n", (int)cr.pid);
    unsigned char *buf = malloc(BIG);
    CHECK(read_all(s, buf, BIG) == 0, "received 300 KiB from the client");
    int ok = 1;
    for (size_t i = 0; i < BIG; i++) if (buf[i] != pattern(i)) { ok = 0; break; }
    CHECK(ok, "data intact");
    CHECK(write_all(s, "thanks", 6) == 0, "reply sent");
    char b[4];
    CHECK(read_all(s, b, 1) < 0, "client hung up: EOF");
    unlink(SOCK_PATH);
    printf("server: %s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}

static int client(void)
{
    who = "client";
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; i++) {           /* the server may not be up yet */
        fd = connect_to(1);
        if (fd < 0) usleep(20000);
    }
    CHECK(fd >= 0, "connected to @" ABSTRACT);
    if (fd < 0) return 1;
    unsigned char *big = malloc(BIG);
    for (size_t i = 0; i < BIG; i++) big[i] = pattern(i);
    CHECK(write_all(fd, big, BIG) == 0, "sent 300 KiB (blocking writes)");
    char b[8] = { 0 };
    CHECK(read_all(fd, b, 6) == 0 && !memcmp(b, "thanks", 6), "got the reply");
    close(fd);
    printf("client: %s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "server")) return server();
    if (argc > 1 && !strcmp(argv[1], "client")) return client();
    int rc = selftest();
    printf("socktest: %s\n", rc ? "FAILED" : "all checks passed");
    return rc;
}
