/* user/eventtest.c -- event APIs: epoll, eventfd, timerfd, signalfd, poll
 * and select sleeping, SCM_RIGHTS descriptor passing, SOCK_SEQPACKET and
 * SOCK_DGRAM socket pairs, POSIX shared memory (/dev/shm).
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 }; nanosleep(&ts, NULL); }

/* ---- epoll ----------------------------------------------------------------------- */
static void test_epoll(void)
{
    printf("epoll:\n");
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0 && (fcntl(ep, F_GETFD) & FD_CLOEXEC), "epoll_create1(EPOLL_CLOEXEC)");
    int p[2];
    pipe(p);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1122334455667788ull }, out[8];
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "EPOLL_CTL_ADD");
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == -1 && errno == EEXIST, "...twice: EEXIST");
    CHECK(epoll_wait(ep, out, 8, 0) == 0, "nothing ready: 0 at once");
    long t0 = ms_now();
    int n = epoll_wait(ep, out, 8, 60);
    long dt = ms_now() - t0;
    CHECK(n == 0 && dt >= 50 && dt < 2000, "timeout of 60 ms honoured");
    write(p[1], "x", 1);
    n = epoll_wait(ep, out, 8, -1);
    CHECK(n == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 0x1122334455667788ull, "readable pipe reported with its data");
    CHECK(epoll_wait(ep, out, 8, 0) == 1, "level-triggered: reported again while readable");

    ev.events = EPOLLIN | EPOLLET;
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0, "EPOLL_CTL_MOD to edge-triggered");
    CHECK(epoll_wait(ep, out, 8, 0) == 1, "ET: first report");
    CHECK(epoll_wait(ep, out, 8, 0) == 0, "ET: not again without new activity");
    char b[16];
    read(p[0], b, sizeof b);                            /* drain: EAGAIN loop in real code */
    write(p[1], "y", 1);
    CHECK(epoll_wait(ep, out, 8, 0) == 1, "ET: new data after draining is reported");

    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev);
    CHECK(epoll_wait(ep, out, 8, 0) == 1 && epoll_wait(ep, out, 8, 0) == 0, "EPOLLONESHOT: once, then disabled");
    epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev);
    CHECK(epoll_wait(ep, out, 8, 0) == 1, "...re-armed by EPOLL_CTL_MOD");
    read(p[0], b, sizeof b);

    /* a write in another process wakes a blocked epoll_wait */
    ev.events = EPOLLIN;
    epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev);
    pid_t pid = fork();
    if (pid == 0) { sleep_ms(50); write(p[1], "z", 1); _exit(0); }
    t0 = ms_now();
    n = epoll_wait(ep, out, 8, 5000);
    dt = ms_now() - t0;
    waitpid(pid, NULL, 0);
    CHECK(n == 1 && dt >= 30 && dt < 1000, "a write in another process wakes epoll_wait");
    read(p[0], b, sizeof b);

    /* EPOLLHUP when the writer closes; the watch leaves with the last close */
    close(p[1]);
    n = epoll_wait(ep, out, 8, 0);
    CHECK(n == 1 && (out[0].events & EPOLLHUP), "writer closed: EPOLLHUP");
    close(p[0]);
    CHECK(epoll_wait(ep, out, 8, 0) == 0, "closing the fd removes its watch");
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == -1 && errno == EBADF, "EPOLL_CTL_DEL of a closed fd: EBADF");

    int fd = open("/tmp/ev-regular", O_CREAT | O_RDWR, 0600);
    ev.events = EPOLLIN;
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == -1 && errno == EPERM, "regular file: EPERM");
    close(fd);
    unlink("/tmp/ev-regular");
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == EINVAL, "adding itself: EINVAL");

    /* nested sets, and loops refused */
    int inner = epoll_create1(0), q[2];
    pipe(q);
    epoll_ctl(inner, EPOLL_CTL_ADD, q[0], &ev);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, inner, &ev) == 0, "an epoll fd inside another");
    errno = 0;
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == ELOOP, "a loop: ELOOP");
    write(q[1], "q", 1);
    CHECK(epoll_wait(ep, out, 8, 0) == 1, "inner readiness seen through the outer set");
    struct pollfd pf = { inner, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN), "poll() on an epoll fd");
    close(q[0]); close(q[1]); close(inner);

    /* many fds, fewer slots than ready ones: everyone gets a turn */
    int pipes[40][2], seen[40] = { 0 };
    for (int i = 0; i < 40; i++) {
        pipe(pipes[i]);
        write(pipes[i][1], "r", 1);
        struct epoll_event e = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
        epoll_ctl(ep, EPOLL_CTL_ADD, pipes[i][0], &e);
    }
    for (int round = 0; round < 5; round++) {
        n = epoll_wait(ep, out, 8, 0);
        for (int i = 0; i < n; i++) if (out[i].data.u32 < 40) seen[out[i].data.u32]++;
    }
    int all = 1;
    for (int i = 0; i < 40; i++) if (seen[i] != 1) all = 0;
    CHECK(all, "40 ready fds, 8 per call: each reported once in 5 calls");
    for (int i = 0; i < 40; i++) { close(pipes[i][0]); close(pipes[i][1]); }
    close(ep);
}

/* ---- eventfd ---------------------------------------------------------------------- */
static void test_eventfd(void)
{
    printf("eventfd:\n");
    int e = eventfd(3, EFD_NONBLOCK);
    uint64_t v = 0;
    CHECK(read(e, &v, 8) == 8 && v == 3, "initial value read");
    errno = 0;
    CHECK(read(e, &v, 8) == -1 && errno == EAGAIN, "empty: EAGAIN");
    v = 5; write(e, &v, 8); v = 7; write(e, &v, 8);
    CHECK(read(e, &v, 8) == 8 && v == 12, "writes add up");
    errno = 0;
    CHECK(read(e, &v, 4) == -1 && errno == EINVAL, "short buffer: EINVAL");
    v = ~0ull;
    errno = 0;
    CHECK(write(e, &v, 8) == -1 && errno == EINVAL, "writing 2^64-1: EINVAL");
    v = 0xfffffffffffffffeull; write(e, &v, 8);
    v = 1;
    errno = 0;
    CHECK(write(e, &v, 8) == -1 && errno == EAGAIN, "full counter: EAGAIN");
    read(e, &v, 8);
    close(e);
    int s = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    uint64_t a = 0, b2 = 0, c = 0;
    int r1 = read(s, &a, 8), r2 = read(s, &b2, 8), r3 = read(s, &c, 8);
    CHECK(r1 == 8 && r2 == 8 && r3 == -1 && a == 1 && b2 == 1, "EFD_SEMAPHORE: one at a time");
    close(s);
    int w = eventfd(0, 0);
    pid_t pid = fork();
    if (pid == 0) { sleep_ms(40); uint64_t one = 1; write(w, &one, 8); _exit(0); }
    long t0 = ms_now();
    CHECK(read(w, &v, 8) == 8 && v == 1 && ms_now() - t0 >= 20, "blocking read woken by a write");
    waitpid(pid, NULL, 0);
    close(w);
}

/* ---- timerfd ---------------------------------------------------------------------- */
static void test_timerfd(void)
{
    printf("timerfd:\n");
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 0 }, { 0, 50 * 1000000 } }, cur;
    long t0 = ms_now();
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0, "one-shot 50 ms armed");
    timerfd_gettime(t, &cur);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 50 * 1000000, "timerfd_gettime: time left");
    uint64_t n = 0;
    CHECK(read(t, &n, 8) == 8 && n == 1, "read blocks until it fires: 1 expiry");
    long dt = ms_now() - t0;
    CHECK(dt >= 45 && dt < 1000, "...after ~50 ms");
    its.it_interval.tv_nsec = 20 * 1000000;
    its.it_value.tv_nsec = 20 * 1000000;
    timerfd_settime(t, 0, &its, NULL);
    sleep_ms(110);
    read(t, &n, 8);
    CHECK(n >= 4 && n <= 6, "periodic 20 ms over 110 ms: ~5 expirations in one read");

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec abs = { { 0, 0 }, now };
    abs.it_value.tv_nsec += 30 * 1000000;
    if (abs.it_value.tv_nsec >= 1000000000) { abs.it_value.tv_sec++; abs.it_value.tv_nsec -= 1000000000; }
    timerfd_settime(t, TFD_TIMER_ABSTIME, &abs, NULL);
    struct pollfd pf = { t, POLLIN, 0 };
    t0 = ms_now();
    int r = poll(&pf, 1, 2000);
    dt = ms_now() - t0;
    CHECK(r == 1 && dt >= 20 && dt < 200, "poll() wakes at an absolute expiry (no polling delay)");
    read(t, &n, 8);

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN }, out;
    epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev);
    struct itimerspec rel = { { 0, 0 }, { 0, 25 * 1000000 } };
    timerfd_settime(t, 0, &rel, NULL);
    t0 = ms_now();
    r = epoll_wait(ep, &out, 1, 2000);
    dt = ms_now() - t0;
    CHECK(r == 1 && dt >= 15 && dt < 200, "epoll_wait wakes when a watched timer fires");
    struct itimerspec off = { { 0, 0 }, { 0, 0 } };
    timerfd_settime(t, 0, &off, NULL);
    timerfd_gettime(t, &cur);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0, "disarmed");
    int fl = fcntl(t, F_GETFL);
    fcntl(t, F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    CHECK(read(t, &n, 8) == -1 && errno == EAGAIN, "disarmed, non-blocking: EAGAIN");
    close(ep);
    close(t);
}

/* ---- signalfd --------------------------------------------------------------------- */
static void test_signalfd(void)
{
    printf("signalfd:\n");
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int s = signalfd(-1, &m, SFD_NONBLOCK);
    struct signalfd_siginfo si;
    errno = 0;
    CHECK(read(s, &si, sizeof si) == -1 && errno == EAGAIN, "nothing pending: EAGAIN");
    kill(getpid(), SIGUSR2);
    struct pollfd pf = { s, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 1, "pending signal: readable");
    CHECK(read(s, &si, sizeof si) == sizeof si && si.ssi_signo == SIGUSR2 && si.ssi_pid == (uint32_t)getpid(),
          "read: SIGUSR2 with the sender's pid");
    sigset_t pend;
    sigpending(&pend);
    CHECK(!sigismember(&pend, SIGUSR2), "...and it is no longer pending");
    int fl = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    pid_t pid = fork();
    if (pid == 0) { sleep_ms(40); kill(getppid(), SIGUSR1); _exit(0); }
    CHECK(read(s, &si, sizeof si) == sizeof si && si.ssi_signo == SIGUSR1, "blocking read woken by a signal from another process");
    waitpid(pid, NULL, 0);
    close(s);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
}

/* ---- poll/select sleep and wake ------------------------------------------------- */
static volatile int got_usr1;
static void on_usr1(int s) { (void)s; got_usr1 = 1; }

static void test_poll(void)
{
    printf("poll, select, ppoll:\n");
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) { sleep_ms(60); write(p[1], "w", 1); _exit(0); }
    struct pollfd pf = { p[0], POLLIN, 0 };
    long t0 = ms_now();
    int r = poll(&pf, 1, 5000);
    long dt = ms_now() - t0;
    CHECK(r == 1 && dt >= 40 && dt < 500, "poll() sleeps until the write");
    waitpid(pid, NULL, 0);
    char b;
    read(p[0], &b, 1);
    pid = fork();
    if (pid == 0) { sleep_ms(60); write(p[1], "w", 1); _exit(0); }
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(p[0], &rs);
    t0 = ms_now();
    r = select(p[0] + 1, &rs, NULL, NULL, NULL);
    dt = ms_now() - t0;
    CHECK(r == 1 && FD_ISSET(p[0], &rs) && dt >= 40 && dt < 500, "select() sleeps until the write");
    waitpid(pid, NULL, 0);
    read(p[0], &b, 1);

    /* ppoll: SIGUSR1 blocked normally, let in only during the wait */
    signal(SIGUSR1, on_usr1);
    sigset_t blk, waitmask;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    kill(getpid(), SIGUSR1);                            /* pending, blocked */
    sigemptyset(&waitmask);
    got_usr1 = 0;
    struct timespec ts = { 2, 0 };
    errno = 0;
    r = ppoll(&pf, 1, &ts, &waitmask);
    int e = errno;
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    CHECK(r == -1 && e == EINTR && got_usr1, "ppoll: signal delivered under the wait's mask, EINTR");
    CHECK(sigismember(&now, SIGUSR1), "...and the old mask is back afterwards");
    sigprocmask(SIG_UNBLOCK, &blk, NULL);
    signal(SIGUSR1, SIG_DFL);
    close(p[0]); close(p[1]);
}

/* ---- SCM_RIGHTS ------------------------------------------------------------------- */
static int send_fds(int sock, const int *fds, int n, const char *data, size_t len)
{
    char cbuf[CMSG_SPACE(sizeof(int) * 16)];
    memset(cbuf, 0, sizeof cbuf);
    struct iovec iov = { (void *)data, len };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = CMSG_SPACE(sizeof(int) * n) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    return (int)sendmsg(sock, &m, 0);
}

static int recv_fds(int sock, int *fds, int max, char *data, size_t len, int *flags, ssize_t *got)
{
    char cbuf[CMSG_SPACE(sizeof(int) * 16)];
    struct iovec iov = { data, len };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                        .msg_controllen = CMSG_SPACE(sizeof(int) * max) };
    *got = recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
    *flags = m.msg_flags;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + n, CMSG_DATA(c), sizeof(int) * k);
            n += k;
        }
    return n;
}

static void test_scm_rights(void)
{
    printf("SCM_RIGHTS:\n");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int p[2];
    pipe(p);
    CHECK(send_fds(sv[0], &p[1], 1, "A", 1) == 1, "sendmsg with one descriptor");
    close(p[1]);                                        /* only the one in flight is left */
    int got[16], flags;
    ssize_t n;
    char d[8];
    int k = recv_fds(sv[1], got, 16, d, sizeof d, &flags, &n);
    CHECK(k == 1 && n == 1 && d[0] == 'A', "received: data and a new descriptor");
    CHECK(k == 1 && (fcntl(got[0], F_GETFD) & FD_CLOEXEC), "MSG_CMSG_CLOEXEC sets close-on-exec");
    write(got[0], "via", 3);
    char rb[4] = { 0 };
    read(p[0], rb, 3);
    CHECK(!strcmp(rb, "via"), "the passed descriptor is the same open pipe");
    close(got[0]);
    CHECK(read(p[0], rb, 1) == 0, "closing it was the last writer: EOF");
    close(p[0]);

    /* across processes, several at once */
    pid_t pid = fork();
    if (pid == 0) {
        int fds[3];
        for (int i = 0; i < 3; i++) {
            char name[32];
            snprintf(name, sizeof name, "/tmp/scm-%d", i);
            fds[i] = open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
            dprintf(fds[i], "file %d", i);
            lseek(fds[i], 0, SEEK_SET);
        }
        _exit(send_fds(sv[0], fds, 3, "BC", 2) == 2 ? 0 : 1);
    }
    int st;
    waitpid(pid, &st, 0);
    k = recv_fds(sv[1], got, 16, d, sizeof d, &flags, &n);
    int ok = k == 3 && n == 2;
    for (int i = 0; ok && i < 3; i++) {
        char want[16], have[16] = { 0 };
        snprintf(want, sizeof want, "file %d", i);
        read(got[i], have, sizeof have - 1);
        ok = !strcmp(want, have);
        close(got[i]);
        snprintf(want, sizeof want, "/tmp/scm-%d", i);
        unlink(want);
    }
    CHECK(ok, "3 files from a child process (which has exited meanwhile)");

    /* descriptors arrive with their own bytes, not earlier ones */
    pipe(p);
    write(sv[0], "xy", 2);
    send_fds(sv[0], &p[0], 1, "Z", 1);
    k = recv_fds(sv[1], got, 16, d, sizeof d, &flags, &n);
    CHECK(k == 0 && n == 2 && !memcmp(d, "xy", 2), "stream: a read stops before the bytes that carry descriptors");
    k = recv_fds(sv[1], got, 16, d, sizeof d, &flags, &n);
    CHECK(k == 1 && n == 1 && d[0] == 'Z', "...which come with the next read");
    if (k == 1) close(got[0]);

    /* no room for them: MSG_CTRUNC, closed */
    send_fds(sv[0], &p[0], 1, "T", 1);
    char plain;
    struct iovec iov = { &plain, 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    CHECK(recvmsg(sv[1], &m, 0) == 1 && (m.msg_flags & MSG_CTRUNC), "no control buffer: MSG_CTRUNC");
    close(p[0]); close(p[1]);
    int bad = 999;
    errno = 0;
    CHECK(send_fds(sv[0], &bad, 1, "E", 1) == -1 && errno == EBADF, "sending a bad descriptor: EBADF");
    close(sv[0]); close(sv[1]);
}

/* ---- SOCK_SEQPACKET, SOCK_DGRAM ------------------------------------------------- */
static void test_records(void)
{
    printf("SOCK_SEQPACKET / SOCK_DGRAM:\n");
    int types[2] = { SOCK_SEQPACKET, SOCK_DGRAM };
    for (int ti = 0; ti < 2; ti++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, types[ti], 0, sv) == 0, ti ? "socketpair(SOCK_DGRAM)" : "socketpair(SOCK_SEQPACKET)");
        send(sv[0], "one", 3, 0);
        send(sv[0], "second", 6, 0);
        send(sv[0], "", 0, 0);
        char b[16] = { 0 };
        ssize_t a = recv(sv[1], b, sizeof b, 0);
        CHECK(a == 3 && !memcmp(b, "one", 3), "records keep their boundaries");
        struct iovec iov = { b, 3 };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
        a = recvmsg(sv[1], &m, 0);
        CHECK(a == 3 && (m.msg_flags & MSG_TRUNC) && !memcmp(b, "sec", 3), "short buffer: truncated, MSG_TRUNC");
        a = recv(sv[1], b, sizeof b, MSG_DONTWAIT);
        CHECK(a == 0, "zero-length record");
        int ty = 0;
        socklen_t tl = sizeof ty;
        getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &ty, &tl);
        CHECK(ty == types[ti], "SO_TYPE");
        close(sv[0]); close(sv[1]);
    }
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, "/tmp/seqpacket.sock");
    unlink(addr.sun_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 4);
    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    int r = connect(c, (struct sockaddr *)&addr, sizeof addr);
    int s = accept(l, NULL, NULL);
    send(c, "hello", 5, 0);
    send(c, "world", 5, 0);
    char b[16] = { 0 };
    CHECK(r == 0 && s >= 0 && recv(s, b, sizeof b, 0) == 5 && !memcmp(b, "hello", 5), "SEQPACKET listen/connect/accept");
    int st = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    CHECK(connect(st, (struct sockaddr *)&addr, sizeof addr) == -1 && errno == EPROTOTYPE, "stream to a seqpacket listener: EPROTOTYPE");
    close(st); close(c); close(s); close(l);
    unlink(addr.sun_path);
}

/* ---- POSIX shared memory ------------------------------------------------------------ */
static void test_shm(void)
{
    printf("shared memory:\n");
    int fd = shm_open("/eventtest", O_CREAT | O_RDWR | O_EXCL, 0600);
    CHECK(fd >= 0, "shm_open creates /dev/shm/eventtest");
    ftruncate(fd, 8192);
    volatile int *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    p[0] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int f2 = shm_open("/eventtest", O_RDWR, 0);
        volatile int *q = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, f2, 0);
        q[1024] = 4242;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    CHECK(p[1024] == 4242, "a write by another process through its own mapping is seen");
    struct stat sb;
    CHECK(stat("/dev/shm/eventtest", &sb) == 0 && sb.st_size == 8192, "visible in /dev/shm with its size");
    CHECK(shm_unlink("/eventtest") == 0 && stat("/dev/shm/eventtest", &sb) == -1, "shm_unlink");
    munmap((void *)p, 8192);
    close(fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("eventtest: pid %d\n", getpid());
    test_epoll();
    test_eventfd();
    test_timerfd();
    test_signalfd();
    test_poll();
    test_scm_rights();
    test_records();
    test_shm();
    if (failures) printf("eventtest: %d check(s) FAILED\n", failures);
    else printf("eventtest: all checks passed\n");
    return failures ? 1 : 0;
}
