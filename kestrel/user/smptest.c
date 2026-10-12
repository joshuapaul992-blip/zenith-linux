/* user/smptest.c -- multiprocessor behaviour: CPUs seen by programs, real
 * parallelism, TLB shootdowns (a thread on another CPU loses an unmapped
 * page at once), page faults on several CPUs at once (resolved without the
 * kernel lock), unix sockets used from several CPUs at once (also without
 * the kernel lock), and the kernel under concurrent system calls.
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- parallelism ------------------------------------------------------------------ */
static volatile uint64_t sink;
static void *spin_work(void *arg)
{
    uint64_t n = (uint64_t)(uintptr_t)arg, x = 1;
    for (uint64_t i = 0; i < n; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
    sink += x;
    return NULL;
}

static long run_threads(int k, uint64_t work)
{
    pthread_t t[16];
    long t0 = ms_now();
    for (int i = 0; i < k; i++) pthread_create(&t[i], NULL, spin_work, (void *)(uintptr_t)work);
    for (int i = 0; i < k; i++) pthread_join(t[i], NULL);
    return ms_now() - t0;
}

/* ---- TLB shootdown ------------------------------------------------------------------- */
static volatile int *volatile shared_page;
static volatile int reader_running, reader_faulted, stop_reader;
static volatile long fault_at;
static sigjmp_buf rjb;
static void on_segv(int s) { (void)s; siglongjmp(rjb, 1); }

static void *reader(void *arg)
{
    (void)arg;
    signal(SIGSEGV, on_segv);
    if (sigsetjmp(rjb, 1)) {                            /* the unmapped page faulted */
        fault_at = ms_now();
        reader_faulted = 1;
        return NULL;
    }
    reader_running = 1;
    uint64_t sum = 0;
    while (!stop_reader) sum += *shared_page;           /* keeps the translation hot */
    sink += sum;
    return NULL;
}

/* ---- thread churn: many threads exiting at once ------------------------------------ */
/* musl releases its thread-list lock through the kernel's clear-child-tid
 * futex at thread exit; a lost or misdirected one hangs the next
 * pthread_create/join for good. */
static void *brief(void *a)
{
    (void)a;
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
    return NULL;
}

/* Returns the threads created and joined, or -1 if a creation failed for
 * any reason but EAGAIN (a full task table is allowed on a busy system). */
static int churn_threads(int rounds)
{
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int total = 0;
    for (int r = 0; r < rounds; r++) {
        pthread_t th[40];
        int k = 0;
        for (int i = 0; i < 40; i++) {
            int e = pthread_create(&th[k], &at, brief, NULL);
            if (e == 0) k++;
            else if (e != EAGAIN) return -1;
        }
        for (int i = 0; i < k; i++) pthread_join(th[i], NULL);
        total += k;
    }
    return total;
}

/* ---- page faults on several CPUs at once ---------------------------------------------
 * Four threads write into the same 8 MiB, each page by all of them, in
 * different orders, so first touches and copy-on-write breaks race; the
 * main thread forks and mprotects meanwhile (both wait for faults in
 * flight). A child forked before checks its copy never changes. */
#define FS_PAGES   2048
#define FS_THREADS 4
#define FS_MARK    0x5EED0000ull
static volatile uint64_t *fs_mem;
static volatile int fs_go;

static void *fault_storm(void *arg)
{
    int id = (int)(intptr_t)arg;
    while (!fs_go) sched_yield();
    for (int p = 0; p < FS_PAGES; p++) {
        int q = (p * 7 + id * 131) % FS_PAGES;
        fs_mem[q * 512 + id] = (uint64_t)q << 8 | (uint64_t)id;
    }
    return NULL;
}

static int fault_round(void)
{
    int bad = 0;
    fs_mem = mmap(NULL, FS_PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *other = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fs_mem == MAP_FAILED || other == MAP_FAILED) return 1;
    for (int p = 0; p < FS_PAGES; p += 4) fs_mem[p * 512 + 15] = FS_MARK + (uint64_t)p;
    int go[2];
    if (pipe(go)) return 1;
    pid_t snap = fork();                                /* every marked page becomes copy-on-write */
    if (snap == 0) {
        char c;
        read(go[0], &c, 1);
        int wrong = 0;
        for (int p = 0; p < FS_PAGES; p++) {
            for (int t = 0; t < FS_THREADS; t++) if (fs_mem[p * 512 + t]) wrong++;
            if (fs_mem[p * 512 + 15] != (p % 4 ? 0 : FS_MARK + (uint64_t)p)) wrong++;
        }
        _exit(wrong ? 1 : 0);
    }
    pthread_t th[FS_THREADS];
    fs_go = 0;
    for (int i = 0; i < FS_THREADS; i++) pthread_create(&th[i], NULL, fault_storm, (void *)(intptr_t)i);
    fs_go = 1;
    for (int i = 0; i < 20; i++) {
        mprotect(other, 16 * 4096, i & 1 ? PROT_READ : PROT_READ | PROT_WRITE);
        pid_t c = fork();
        if (c == 0) _exit(0);
        if (c > 0) waitpid(c, NULL, 0);
    }
    for (int i = 0; i < FS_THREADS; i++) pthread_join(th[i], NULL);
    for (int p = 0; p < FS_PAGES; p++) {
        for (int t = 0; t < FS_THREADS; t++) if (fs_mem[p * 512 + t] != ((uint64_t)p << 8 | (uint64_t)t)) bad++;
        if (fs_mem[p * 512 + 15] != (p % 4 ? 0 : FS_MARK + (uint64_t)p)) bad++;
    }
    write(go[1], "x", 1);
    int st = 0;
    if (waitpid(snap, &st, 0) != snap || !WIFEXITED(st) || WEXITSTATUS(st)) bad++;
    close(go[0]);
    close(go[1]);
    munmap((void *)fs_mem, FS_PAGES * 4096);
    munmap(other, 16 * 4096);
    return bad;
}

/* ---- unix sockets from several CPUs ---------------------------------------------------- */
#define US_PAIRS 4
#define US_BYTES (2 << 20)
static volatile int us_errors;

static void *us_writer(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static __thread unsigned char buf[9000];
    unsigned seed = (unsigned)fd * 2654435761u;
    for (long pos = 0; pos < US_BYTES; ) {
        seed = seed * 1103515245u + 12345u;
        long n = 1 + (long)(seed >> 8) % (long)sizeof buf;
        if (n > US_BYTES - pos) n = US_BYTES - pos;
        for (long i = 0; i < n; i++) buf[i] = (unsigned char)((pos + i) % 251);
        for (long off = 0; off < n; ) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w <= 0) { us_errors++; return NULL; }
            off += w;
        }
        pos += n;
    }
    return NULL;
}

static void *us_reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static __thread unsigned char buf[7000];
    long pos = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { us_errors++; break; }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++) if (buf[i] != (unsigned char)((pos + i) % 251)) { us_errors++; return NULL; }
        pos += r;
    }
    if (pos != US_BYTES) us_errors++;
    return NULL;
}

/* Records carrying a descriptor: a pipe whose content names the record. */
static void *us_fd_sender(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (int i = 0; i < 300; i++) {
        int p[2];
        if (pipe(p)) { us_errors++; break; }
        write(p[1], &i, sizeof i);
        close(p[1]);
        char ctl[CMSG_SPACE(sizeof(int))];
        struct iovec v = { &i, sizeof i };
        struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &p[0], sizeof(int));
        if (sendmsg(fd, &m, 0) != sizeof i) us_errors++;
        close(p[0]);
    }
    shutdown(fd, SHUT_WR);
    return NULL;
}

static void *us_fd_receiver(void *arg)
{
    int fd = (int)(intptr_t)arg, got = 0;
    for (;;) {
        int idx = -1;
        char ctl[CMSG_SPACE(sizeof(int))];
        struct iovec v = { &idx, sizeof idx };
        struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
        ssize_t r = recvmsg(fd, &m, 0);
        if (r == 0) break;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        if (r != sizeof idx || !c || c->cmsg_type != SCM_RIGHTS) { us_errors++; break; }
        int pfd, inside = -2;
        memcpy(&pfd, CMSG_DATA(c), sizeof pfd);
        if (read(pfd, &inside, sizeof inside) != sizeof inside || inside != idx || idx != got) us_errors++;
        close(pfd);
        got++;
    }
    if (got != 300) us_errors++;
    return NULL;
}

/* A reader blocked in recv on one CPU while the peer closes on another. */
static void *us_eof_reader(void *arg)
{
    char c;
    if (read((int)(intptr_t)arg, &c, 1) != 0) us_errors++;
    return NULL;
}

static int unix_round(void)
{
    us_errors = 0;
    pthread_t w[US_PAIRS], r[US_PAIRS], fs, fr;
    int sv[US_PAIRS][2], fv[2];
    for (int i = 0; i < US_PAIRS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i])) return 1;
        pthread_create(&w[i], NULL, us_writer, (void *)(intptr_t)sv[i][0]);
        pthread_create(&r[i], NULL, us_reader, (void *)(intptr_t)sv[i][1]);
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fv)) return 1;
    pthread_create(&fs, NULL, us_fd_sender, (void *)(intptr_t)fv[0]);
    pthread_create(&fr, NULL, us_fd_receiver, (void *)(intptr_t)fv[1]);
    for (int i = 0; i < US_PAIRS; i++) { pthread_join(w[i], NULL); close(sv[i][0]); }
    for (int i = 0; i < US_PAIRS; i++) { pthread_join(r[i], NULL); close(sv[i][1]); }
    pthread_join(fs, NULL);
    pthread_join(fr, NULL);
    close(fv[0]); close(fv[1]);
    for (int i = 0; i < 200; i++) {
        int e[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, e)) { us_errors++; break; }
        pthread_t t;
        pthread_create(&t, NULL, us_eof_reader, (void *)(intptr_t)e[1]);
        if (i & 1) sched_yield();
        close(e[0]);
        pthread_join(t, NULL);
        close(e[1]);
    }
    return us_errors;
}

/* ---- pipes, eventfds and poll from several CPUs --------------------------------------- */
#define PP_PIPES 4
#define PP_BYTES (1 << 20)
#define EV_THREADS 4
#define EV_ADDS 5000

static void *pp_reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    unsigned char buf[5000];
    long pos = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 5000) != 1) { us_errors++; break; }
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EAGAIN) continue;         /* another wakeup took it: poll again */
        if (r < 0) { us_errors++; break; }
        if (r == 0) break;
        for (ssize_t i = 0; i < r; i++) if (buf[i] != (unsigned char)((pos + i) % 251)) { us_errors++; return NULL; }
        pos += r;
    }
    if (pos != PP_BYTES) us_errors++;
    return NULL;
}

static void *pp_writer(void *arg)
{
    int fd = (int)(intptr_t)arg;
    unsigned char buf[3000];
    for (long pos = 0; pos < PP_BYTES; ) {
        long n = PP_BYTES - pos < (long)sizeof buf ? PP_BYTES - pos : (long)sizeof buf;
        for (long i = 0; i < n; i++) buf[i] = (unsigned char)((pos + i) % 251);
        for (long off = 0; off < n; ) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w <= 0) { us_errors++; return NULL; }
            off += w;
        }
        pos += n;
    }
    close(fd);
    return NULL;
}

static void *ev_adder(void *arg)
{
    int fd = (int)(intptr_t)arg;
    for (int i = 0; i < EV_ADDS; i++) {
        uint64_t one = 1;
        if (write(fd, &one, 8) != 8) { us_errors++; break; }
        if (!(i & 63)) sched_yield();
    }
    return NULL;
}

/* Edge-triggered epoll over socket pairs written from other CPUs: read
 * until EAGAIN after each report, as event loops do. */
#define EP_PAIRS 4
#define EP_BYTES (512 << 10)
static void *ep_writer(void *arg)
{
    int fd = (int)(intptr_t)arg;
    unsigned char buf[1500];
    for (long pos = 0; pos < EP_BYTES; ) {
        long n = EP_BYTES - pos < (long)sizeof buf ? EP_BYTES - pos : (long)sizeof buf;
        for (long i = 0; i < n; i++) buf[i] = (unsigned char)((pos + i) % 251);
        ssize_t w = write(fd, buf, (size_t)n);
        if (w < 0 && errno == EAGAIN) { sched_yield(); continue; }
        if (w <= 0) { us_errors++; return NULL; }
        pos += w;
    }
    return NULL;
}

static int epoll_round(void)
{
    us_errors = 0;
    int sv[EP_PAIRS][2], ep = epoll_create1(0);
    long got[EP_PAIRS] = { 0 };
    pthread_t w[EP_PAIRS];
    if (ep < 0) return 1;
    for (int i = 0; i < EP_PAIRS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv[i])) return 1;
        struct epoll_event e = { .events = EPOLLIN | EPOLLET, .data.u32 = (uint32_t)i };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[i][1], &e)) return 1;
    }
    for (int i = 0; i < EP_PAIRS; i++) pthread_create(&w[i], NULL, ep_writer, (void *)(intptr_t)sv[i][0]);
    long total = 0;
    unsigned char buf[4096];
    while (total < (long)EP_PAIRS * EP_BYTES) {
        struct epoll_event ev[EP_PAIRS];
        int n = epoll_wait(ep, ev, EP_PAIRS, 5000);
        if (n <= 0) { us_errors++; break; }
        for (int k = 0; k < n; k++) {
            int i = (int)ev[k].data.u32;
            for (;;) {
                ssize_t r = read(sv[i][1], buf, sizeof buf);
                if (r < 0 && errno == EAGAIN) break;
                if (r <= 0) { us_errors++; break; }
                for (ssize_t j = 0; j < r; j++) if (buf[j] != (unsigned char)((got[i] + j) % 251)) { us_errors++; break; }
                got[i] += r;
                total += r;
            }
        }
    }
    for (int i = 0; i < EP_PAIRS; i++) { pthread_join(w[i], NULL); close(sv[i][0]); close(sv[i][1]); }
    close(ep);
    return us_errors;
}

static int pipe_poll_round(void)
{
    us_errors = 0;
    pthread_t w[PP_PIPES], r[PP_PIPES], ev[EV_THREADS];
    int pv[PP_PIPES][2];
    for (int i = 0; i < PP_PIPES; i++) {
        if (pipe(pv[i])) return 1;
        fcntl(pv[i][0], F_SETFL, O_NONBLOCK);
        pthread_create(&r[i], NULL, pp_reader, (void *)(intptr_t)pv[i][0]);
        pthread_create(&w[i], NULL, pp_writer, (void *)(intptr_t)pv[i][1]);
    }
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) return 1;
    for (int i = 0; i < EV_THREADS; i++) pthread_create(&ev[i], NULL, ev_adder, (void *)(intptr_t)efd);
    uint64_t sum = 0;
    while (sum < (uint64_t)EV_THREADS * EV_ADDS) {
        struct pollfd p = { efd, POLLIN, 0 };
        if (poll(&p, 1, 5000) != 1) { us_errors++; break; }
        uint64_t v;
        if (read(efd, &v, 8) == 8) sum += v;
    }
    if (sum != (uint64_t)EV_THREADS * EV_ADDS) us_errors++;
    for (int i = 0; i < EV_THREADS; i++) pthread_join(ev[i], NULL);
    close(efd);
    for (int i = 0; i < PP_PIPES; i++) { pthread_join(w[i], NULL); pthread_join(r[i], NULL); close(pv[i][0]); }
    return us_errors;
}

/* ---- regular files: one open file shared by several CPUs ---------------------------- */
#define RF_THREADS  4
#define RF_RECS     1500                /* records per thread */
#define RF_REC      64
static const char *rf_path;
static int rf_fd;
static volatile int rf_errors;
static const char *volatile rf_why;           /* the first failure */
static volatile long rf_got, rf_at;
#define RF_FAIL(what, got, at) do { if (!rf_errors) { rf_why = (what); rf_got = (long)(got); rf_at = (long)(at); } rf_errors++; } while (0)

static void rf_fill(unsigned char *r, int t, int seq)
{
    memset(r, 0, RF_REC);
    r[0] = 0xA5; r[1] = (unsigned char)t; r[2] = (unsigned char)seq; r[3] = (unsigned char)(seq >> 8);
    for (int i = 4; i < RF_REC; i++) r[i] = (unsigned char)(t * 31 + seq * 7 + i);
}
static int rf_ok(const unsigned char *r, int *t, int *seq)
{
    if (r[0] != 0xA5 || r[1] >= RF_THREADS) return 0;
    *t = r[1]; *seq = r[2] | r[3] << 8;
    if (*seq >= RF_RECS) return 0;
    unsigned char e[RF_REC];
    rf_fill(e, *t, *seq);
    return !memcmp(e, r, RF_REC);
}
static void *rf_writer(void *arg)             /* write() on the shared descriptor */
{
    int t = (int)(intptr_t)arg;
    unsigned char r[RF_REC];
    for (int i = 0; i < RF_RECS; i++) {
        rf_fill(r, t, i);
        { ssize_t n = write(rf_fd, r, RF_REC); if (n != RF_REC) RF_FAIL("write", n < 0 ? -errno : n, i); }
        if (i % 500 == 250 && fsync(rf_fd)) RF_FAIL("fsync", -errno, i);
    }
    return NULL;
}
static void *rf_appender(void *arg)           /* write() with O_APPEND, each on its own descriptor */
{
    int t = (int)(intptr_t)arg;
    int fd = open(rf_path, O_WRONLY | O_APPEND);
    if (fd < 0) { RF_FAIL("open O_APPEND", -errno, 0); return NULL; }
    unsigned char r[RF_REC];
    for (int i = 0; i < RF_RECS; i++) {
        rf_fill(r, t, i);
        { ssize_t n = write(fd, r, RF_REC); if (n != RF_REC) RF_FAIL("append write", n < 0 ? -errno : n, i); }
    }
    close(fd);
    return NULL;
}
static unsigned char rf_seen[RF_THREADS][RF_RECS];
static void *rf_reader(void *arg)             /* read() on the shared descriptor */
{
    (void)arg;
    unsigned char r[RF_REC];
    for (;;) {
        ssize_t n = read(rf_fd, r, RF_REC);
        if (n == 0) break;
        int t, seq;
        if (n != RF_REC || !rf_ok(r, &t, &seq)) { RF_FAIL(n != RF_REC ? "read: short" : "read: bad record", n < 0 ? -errno : n, lseek(rf_fd, 0, SEEK_CUR)); break; }
        __atomic_add_fetch(&rf_seen[t][seq], 1, __ATOMIC_RELAXED);
        { off_t o = lseek(rf_fd, 0, SEEK_CUR); if (o % RF_REC) RF_FAIL("lseek mid-record", o, 0); }      /* never mid-record */
    }
    return NULL;
}
static void *rf_pwriter(void *arg)            /* pwrite() of a region of its own */
{
    int t = (int)(intptr_t)arg;
    unsigned char r[RF_REC];
    for (int i = 0; i < RF_RECS; i++) {
        rf_fill(r, t, i);
        { ssize_t n = pwrite(rf_fd, r, RF_REC, ((off_t)t * RF_RECS + i) * RF_REC); if (n != RF_REC) RF_FAIL("pwrite", n < 0 ? -errno : n, i); }
    }
    return NULL;
}
static volatile int rf_stop;
static void *rf_statter(void *arg)            /* fstat as the file grows: never shrinks */
{
    (void)arg;
    off_t last = 0;
    while (!rf_stop) {
        struct stat st;
        if (fstat(rf_fd, &st) || st.st_size < last || st.st_size % RF_REC) { RF_FAIL("fstat size", st.st_size, last); break; }
        last = st.st_size;
        unsigned char r[RF_REC];
        if (last) { ssize_t n = pread(rf_fd, r, RF_REC, last - RF_REC); if (n != RF_REC) RF_FAIL("pread of the last record", n < 0 ? -errno : n, last); }
    }
    return NULL;
}

/* The whole file: every thread's every record exactly once, anywhere if
 * !in_place, else at its own place. */
static int rf_check(int in_place)
{
    int errs = 0;
    struct stat st;
    if (fstat(rf_fd, &st) || st.st_size != (off_t)RF_THREADS * RF_RECS * RF_REC) return 1;
    memset(rf_seen, 0, sizeof rf_seen);
    static unsigned char buf[RF_THREADS * RF_RECS * RF_REC];
    if (pread(rf_fd, buf, sizeof buf, 0) != (ssize_t)sizeof buf) return 1;
    for (int k = 0; k < RF_THREADS * RF_RECS; k++) {
        int t, seq;
        if (!rf_ok(buf + (size_t)k * RF_REC, &t, &seq)) { errs++; continue; }
        if (in_place && k != t * RF_RECS + seq) errs++;
        rf_seen[t][seq]++;
    }
    for (int t = 0; t < RF_THREADS; t++)
        for (int i = 0; i < RF_RECS; i++) if (rf_seen[t][i] != 1) errs++;
    return errs;
}

static int file_round(const char *path)
{
    pthread_t w[RF_THREADS], x;
    int bad = 0;
    rf_path = path;
    rf_errors = 0;
    /* 1: write() through one descriptor, with fsync and fstat alongside */
    if ((rf_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0) return 1;
    rf_stop = 0;
    pthread_create(&x, NULL, rf_statter, NULL);
    for (int i = 0; i < RF_THREADS; i++) pthread_create(&w[i], NULL, rf_writer, (void *)(intptr_t)i);
    for (int i = 0; i < RF_THREADS; i++) pthread_join(w[i], NULL);
    rf_stop = 1;
    pthread_join(x, NULL);
    if (rf_check(0)) { printf("  FAIL detail: shared write(): records lost or torn\n"); bad++; }
    /* 2: read() through the same descriptor: each record by exactly one thread */
    memset(rf_seen, 0, sizeof rf_seen);
    lseek(rf_fd, 0, SEEK_SET);
    for (int i = 0; i < RF_THREADS; i++) pthread_create(&w[i], NULL, rf_reader, NULL);
    for (int i = 0; i < RF_THREADS; i++) pthread_join(w[i], NULL);
    for (int t = 0; t < RF_THREADS; t++)
        for (int i = 0; i < RF_RECS; i++)
            if (rf_seen[t][i] != 1) { printf("  FAIL detail: shared read(): a record read %d times\n", rf_seen[t][i]); bad++; t = RF_THREADS; break; }
    /* 3: O_APPEND from four descriptors */
    if (ftruncate(rf_fd, 0)) bad++;
    for (int i = 0; i < RF_THREADS; i++) pthread_create(&w[i], NULL, rf_appender, (void *)(intptr_t)i);
    for (int i = 0; i < RF_THREADS; i++) pthread_join(w[i], NULL);
    if (rf_check(0)) { printf("  FAIL detail: O_APPEND: records lost or torn\n"); bad++; }
    /* 4: pwrite() of separate regions */
    if (ftruncate(rf_fd, 0)) bad++;
    for (int i = 0; i < RF_THREADS; i++) pthread_create(&w[i], NULL, rf_pwriter, (void *)(intptr_t)i);
    for (int i = 0; i < RF_THREADS; i++) pthread_join(w[i], NULL);
    if (rf_check(1)) { printf("  FAIL detail: pwrite(): records lost or misplaced\n"); bad++; }
    close(rf_fd);
    unlink(path);
    if (rf_errors) printf("  FAIL detail: %d call(s) failed; first: %s gave %ld at %ld\n", rf_errors, rf_why, rf_got, rf_at);
    return bad + rf_errors;
}

/* ---- concurrent system calls ------------------------------------------------------- */
static volatile int churn_errors;
static void *churn(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 300; i++) {
        char *p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { churn_errors++; continue; }
        memset(p, id, 3 * 4096);
        if (p[4096 + 7] != (char)id) churn_errors++;
        munmap(p, 3 * 4096);
        int fds[2];
        if (pipe(fds) == 0) {
            char c = (char)id, d = 0;
            write(fds[1], &c, 1);
            read(fds[0], &d, 1);
            if (d != c) churn_errors++;
            close(fds[0]);
            close(fds[1]);
        }
        if (getpid() <= 0) churn_errors++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "files")) {          /* smptest files N: only the file rounds */
        int n = atoi(argv[2]);
        for (int i = 0; i < n; i++) {
            CHECK(file_round("/var/tmp/smptest.dat") == 0, "ext2 file round");
            CHECK(file_round("/tmp/smptest.dat") == 0, "tmpfs file round");
        }
        printf("smptest files: %d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    printf("smptest: pid %d, %ld CPU(s) online\n", getpid(), n);
    CHECK(n >= 1 && n <= 16, "sysconf(_SC_NPROCESSORS_ONLN)");
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof cs, &cs);
    CHECK(CPU_COUNT(&cs) == n, "sched_getaffinity lists every online CPU");

    printf("parallelism:\n");
    int k = n > 4 ? 4 : (int)n;
    uint64_t work = 30000000;
    long one = run_threads(1, work);
    long many = run_threads(k, work);                   /* k times the work */
    printf("  1 thread: %ld ms, %d threads (%dx the work): %ld ms\n", one, k, k, many);
    if (k > 1) CHECK(many < one * k * 3 / 4, "threads run on several CPUs at once (speed-up)");

    printf("TLB shootdown:\n");
    for (int round = 0; round < 5; round++) {
        int *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *p = 7;
        shared_page = p;
        reader_running = reader_faulted = stop_reader = 0;
        pthread_t r;
        pthread_create(&r, NULL, reader, NULL);
        while (!reader_running) sched_yield();
        struct timespec ts = { 0, 20 * 1000000 };
        nanosleep(&ts, NULL);                           /* reading away on its CPU */
        long t0 = ms_now();
        munmap(p, 4096);
        for (int i = 0; i < 200 && !reader_faulted; i++) nanosleep(&ts, NULL);
        stop_reader = 1;
        pthread_join(r, NULL);
        if (!reader_faulted || fault_at - t0 > 1000) {
            CHECK(0, "a thread reading a page faults as soon as another thread unmaps it");
            break;
        }
        if (round == 4) CHECK(1, "a thread reading a page faults as soon as another thread unmaps it (5 rounds)");
    }

    printf("page faults on several CPUs:\n");
    int fbad = 0;
    for (int r = 0; r < 3; r++) fbad += fault_round();
    CHECK(fbad == 0, "4 threads x 8 MiB of racing first touches and copy-on-write, forks alongside (3 rounds)");

    printf("unix sockets on several CPUs:\n");
    int ubad = 0;
    for (int r = 0; r < 2; r++) ubad += unix_round();
    CHECK(ubad == 0, "4 streams x 2 MiB checked byte by byte, 300 records with descriptors, 200 close-while-reading (2 rounds)");

    printf("pipes, eventfds and poll on several CPUs:\n");
    int pbad = 0;
    for (int r = 0; r < 2; r++) pbad += pipe_poll_round();
    CHECK(pbad == 0, "4 pipes x 1 MiB read after poll, 4 threads x 5000 eventfd adds summed (2 rounds)");
    int ebad = 0;
    for (int r = 0; r < 2; r++) ebad += epoll_round();
    CHECK(ebad == 0, "4 sockets x 512 KiB through edge-triggered epoll, read to EAGAIN (2 rounds)");

    printf("regular files shared by several CPUs:\n");
    CHECK(file_round("/var/tmp/smptest.dat") == 0,
          "ext2: 4 threads x 1500 records by write(), read(), O_APPEND and pwrite(), fstat and fsync alongside");
    CHECK(file_round("/tmp/smptest.dat") == 0,
          "tmpfs: 4 threads x 1500 records by write(), read(), O_APPEND and pwrite(), fstat alongside");

    printf("thread churn:\n");
    int procs = 3, okp = 0;
    for (int i = 0; i < procs; i++)
        if (fork() == 0) { int t = churn_threads(60); _exit(t >= 60 * 40 / 2 ? 0 : 1); }
    for (int i = 0; i < procs; i++) { int s2; if (wait(&s2) > 0 && WIFEXITED(s2) && !WEXITSTATUS(s2)) okp++; }
    CHECK(okp == procs, "3 processes x 2400 threads created, exited and joined on all CPUs (no hang)");

    printf("concurrent system calls:\n");
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, churn, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    CHECK(churn_errors == 0, "8 threads x 300 rounds of mmap/munmap/pipe/getpid");
    int st = 0, bad = 0;
    for (int i = 0; i < 8; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            pthread_t c[2];
            for (int j = 0; j < 2; j++) pthread_create(&c[j], NULL, churn, (void *)(intptr_t)(j + 20));
            for (int j = 0; j < 2; j++) pthread_join(c[j], NULL);
            _exit(churn_errors ? 1 : 0);
        }
    }
    for (int i = 0; i < 8; i++) { if (wait(&st) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) bad++; }
    CHECK(bad == 0, "8 processes x 2 threads of the same at once");

    if (failures) printf("smptest: %d check(s) FAILED\n", failures);
    else printf("smptest: all checks passed\n");
    return failures ? 1 : 0;
}
