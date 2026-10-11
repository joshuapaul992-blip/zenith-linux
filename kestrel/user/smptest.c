/* user/smptest.c -- multiprocessor behaviour: CPUs seen by programs, real
 * parallelism, TLB shootdowns (a thread on another CPU loses an unmapped
 * page at once), page faults on several CPUs at once (resolved without the
 * kernel lock), and the kernel under concurrent system calls.
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

int main(void)
{
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
