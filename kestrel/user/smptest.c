/* user/smptest.c -- multiprocessor behaviour: CPUs seen by programs, real
 * parallelism, TLB shootdowns (a thread on another CPU loses an unmapped
 * page at once), and the kernel under concurrent system calls.
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

static int churn_threads(int rounds)
{
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int total = 0;
    for (int r = 0; r < rounds; r++) {
        pthread_t th[40];
        int k = 0;
        for (int i = 0; i < 40; i++) if (pthread_create(&th[k], &at, brief, NULL) == 0) k++;
        for (int i = 0; i < k; i++) pthread_join(th[i], NULL);
        total += k;
    }
    return total;
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

    printf("thread churn:\n");
    int procs = 3, okp = 0;
    for (int i = 0; i < procs; i++)
        if (fork() == 0) _exit(churn_threads(60) == 60 * 40 ? 0 : 1);
    for (int i = 0; i < procs; i++) { int s2; if (wait(&s2) > 0 && WIFEXITED(s2) && !WEXITSTATUS(s2)) okp++; }
    CHECK(okp == procs, "3 processes x 2400 threads created, exited and joined on all CPUs");

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
