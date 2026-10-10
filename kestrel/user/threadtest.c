/* user/threadtest.c -- threads: pthreads on clone(CLONE_THREAD), TLS,
 * futexes, thread-directed and process-directed signals, exit/exit_group,
 * fork and execve from a thread, robust mutexes.
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
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* <linux/futex.h> (no kernel headers in the musl sysroot) */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_OP_PRIVATE (FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_BITSET_MATCH_ANY 0xffffffff
#define FUTEX_OP_ADD 1
#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    (((op & 0xf) << 28) | ((cmp & 0xf) << 24) | ((oparg & 0xfff) << 12) | (cmparg & 0xfff))

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static long futex(volatile int *a, int op, int val, const struct timespec *ts, volatile int *a2, int val3)
{
    return syscall(SYS_futex, a, op, val, ts, a2, val3);
}
static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }
static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 }; nanosleep(&ts, NULL); }

/* ---- create / join, ids, TLS ------------------------------------------------- */
static __thread int tls_value = 42;
static __thread char tls_buf[64];
static pid_t tids[16];

static void *basic(void *arg)
{
    int i = (int)(intptr_t)arg;
    tids[i] = gettid_();
    if (tls_value != 42) return (void *)-1;             /* fresh TLS image in every thread */
    tls_value = 1000 + i;
    snprintf(tls_buf, sizeof tls_buf, "thread %d", i);
    sched_yield();
    sleep_ms(5);
    char want[64];
    snprintf(want, sizeof want, "thread %d", i);
    if (tls_value != 1000 + i || strcmp(tls_buf, want)) return (void *)-2;
    if (getpid() != (pid_t)syscall(SYS_getpid)) return (void *)-3;
    return (void *)(intptr_t)(i * 3);
}

static void test_basic(void)
{
    printf("create, join, TLS:\n");
    pthread_t th[16];
    int created = 0, joined_ok = 0;
    for (int i = 0; i < 16; i++) if (pthread_create(&th[i], NULL, basic, (void *)(intptr_t)i) == 0) created++;
    CHECK(created == 16, "16 threads created");
    for (int i = 0; i < created; i++) {
        void *r;
        if (pthread_join(th[i], &r) == 0 && r == (void *)(intptr_t)(i * 3)) joined_ok++;
    }
    CHECK(joined_ok == 16, "every thread joined with its value, TLS private and intact");
    int distinct = 1;
    for (int i = 0; i < 16; i++) {
        if (tids[i] <= 0 || tids[i] == getpid()) distinct = 0;
        for (int j = 0; j < i; j++) if (tids[i] == tids[j]) distinct = 0;
    }
    CHECK(distinct, "each thread has its own tid, none equal to the pid");
    CHECK(tls_value == 42, "main thread's TLS untouched");
    CHECK(gettid_() == getpid(), "main thread: tid == pid");
}

/* ---- mutex, condvar, semaphore ------------------------------------------------ */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static void *incr(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&mtx);
        long v = counter;
        if ((i & 255) == 0) sched_yield();              /* hold the lock across a switch */
        counter = v + 1;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int queue[8], qn, produced_sum, consumed_sum, done_producing;
static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 2000; i++) {
        pthread_mutex_lock(&mtx);
        while (qn == 8) pthread_cond_wait(&cv, &mtx);
        queue[qn++] = i;
        produced_sum += i;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
    }
    pthread_mutex_lock(&mtx);
    done_producing++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    return NULL;
}
static void *consumer(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mtx);
        while (qn == 0 && done_producing < 2) pthread_cond_wait(&cv, &mtx);
        if (qn == 0) { pthread_mutex_unlock(&mtx); return NULL; }
        consumed_sum += queue[--qn];
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
    }
}

static sem_t sem;
static void *sem_poster(void *arg) { (void)arg; for (int i = 0; i < 100; i++) { sem_post(&sem); if (i % 7 == 0) sched_yield(); } return NULL; }

static void test_sync(void)
{
    printf("mutex, condition variable, semaphore:\n");
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, incr, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    CHECK(counter == 8 * 20000, "8 threads x 20000 locked increments: no lost update");

    pthread_t p[2], c[3];
    for (int i = 0; i < 2; i++) pthread_create(&p[i], NULL, producer, NULL);
    for (int i = 0; i < 3; i++) pthread_create(&c[i], NULL, consumer, NULL);
    for (int i = 0; i < 2; i++) pthread_join(p[i], NULL);
    for (int i = 0; i < 3; i++) pthread_join(c[i], NULL);
    CHECK(produced_sum == consumed_sum && produced_sum == 2 * 2001000, "bounded queue: 2 producers, 3 consumers");

    sem_init(&sem, 0, 0);
    pthread_t sp;
    pthread_create(&sp, NULL, sem_poster, NULL);
    int got = 0;
    for (int i = 0; i < 100; i++) if (sem_wait(&sem) == 0) got++;
    pthread_join(sp, NULL);
    CHECK(got == 100, "semaphore: 100 posts, 100 waits");

    pthread_mutex_lock(&mtx);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 60 * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    long t0 = ms_now();
    int r = pthread_cond_timedwait(&cv, &mtx, &ts);
    long dt = ms_now() - t0;
    pthread_mutex_unlock(&mtx);
    CHECK(r == ETIMEDOUT && dt >= 40 && dt < 2000, "pthread_cond_timedwait times out");
}

/* ---- futex system call -------------------------------------------------------- */
static volatile int fw1, fw2;
static volatile int fwaiting;
static void *fwaiter(void *arg)
{
    volatile int *w = arg;
    __atomic_add_fetch(&fwaiting, 1, __ATOMIC_SEQ_CST);
    while (*w == 0) futex(w, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    return NULL;
}

static void wait_for_sleepers(int n)
{
    while (__atomic_load_n(&fwaiting, __ATOMIC_SEQ_CST) < n) sched_yield();
    sleep_ms(20);                                       /* and actually asleep in the kernel */
}

static void test_futex(void)
{
    printf("futex:\n");
    volatile int f = 1;
    errno = 0;
    CHECK(futex(&f, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0) == -1 && errno == EAGAIN, "WAIT on a changed value: EAGAIN");
    struct timespec ts = { 0, 50 * 1000000 };
    long t0 = ms_now();
    errno = 0;
    long r = futex(&f, FUTEX_WAIT_PRIVATE, 1, &ts, NULL, 0);
    long dt = ms_now() - t0;
    CHECK(r == -1 && errno == ETIMEDOUT && dt >= 45 && dt < 2000, "WAIT with a 50 ms timeout: ETIMEDOUT on time");
    errno = 0;
    CHECK(futex((volatile int *)((char *)&f + 1), FUTEX_WAIT_PRIVATE, 1, &ts, NULL, 0) == -1 && errno == EINVAL,
          "unaligned futex: EINVAL");
    errno = 0;
    CHECK(futex((volatile int *)16, FUTEX_WAIT_PRIVATE, 1, &ts, NULL, 0) == -1 && errno == EFAULT, "unmapped futex: EFAULT");
    CHECK(futex(&f, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) == 0, "WAKE with no waiters: 0");
    errno = 0;
    CHECK(futex(&f, FUTEX_LOCK_PI, 0, NULL, NULL, 0) == -1 && errno == ENOSYS, "PI futexes: ENOSYS");

    /* requeue: two waiters on fw1 move to fw2, then wake there */
    pthread_t a, b;
    fwaiting = 0;
    pthread_create(&a, NULL, fwaiter, (void *)&fw1);
    pthread_create(&b, NULL, fwaiter, (void *)&fw1);
    wait_for_sleepers(2);
    errno = 0;
    CHECK(futex(&fw1, FUTEX_CMP_REQUEUE_PRIVATE, 0, (void *)(uintptr_t)2, &fw2, 1) == -1 && errno == EAGAIN,
          "CMP_REQUEUE with a wrong value: EAGAIN");
    CHECK(futex(&fw1, FUTEX_CMP_REQUEUE_PRIVATE, 0, (void *)(uintptr_t)2, &fw2, 0) == 2, "CMP_REQUEUE moves 2 waiters");
    CHECK(futex(&fw1, FUTEX_WAKE_PRIVATE, 10, NULL, NULL, 0) == 0, "nobody left on the first futex");
    fw1 = 1;                                            /* the waiters re-check *w == fw1's value... */
    CHECK(futex(&fw2, FUTEX_WAKE_PRIVATE, 10, NULL, NULL, 0) == 2, "both woken on the second futex");
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    /* WAKE_OP: fw2 += 1 (old 0 == 0: wake on both) */
    volatile int w3 = 0;
    fw2 = 0;
    fwaiting = 0;
    pthread_create(&a, NULL, fwaiter, (void *)&w3);
    wait_for_sleepers(1);
    w3 = 1;
    int op = FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0);
    CHECK(futex(&w3, FUTEX_WAKE_OP_PRIVATE, 1, (void *)(uintptr_t)1, &fw2, op) == 1 && fw2 == 1,
          "WAKE_OP: atomic add on the second word, waiter woken");
    pthread_join(a, NULL);

    /* WAIT_BITSET with an absolute CLOCK_MONOTONIC deadline */
    struct timespec abs;
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_nsec += 30 * 1000000;
    if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
    t0 = ms_now();
    errno = 0;
    r = futex(&f, FUTEX_WAIT_BITSET_PRIVATE, 1, &abs, NULL, FUTEX_BITSET_MATCH_ANY);
    dt = ms_now() - t0;
    CHECK(r == -1 && errno == ETIMEDOUT && dt >= 20 && dt < 2000, "WAIT_BITSET absolute deadline");

    /* a shared futex across processes (MAP_SHARED) */
    volatile int *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *sh = 0;
    pid_t pid = fork();
    if (pid == 0) {
        while (*sh == 0) futex(sh, FUTEX_WAIT, 0, NULL, NULL, 0);
        _exit(*sh == 7 ? 0 : 1);
    }
    sleep_ms(50);
    *sh = 7;
    long woke = futex(sh, FUTEX_WAKE, 1, NULL, NULL, 0);
    int st = -1;
    waitpid(pid, &st, 0);
    CHECK(woke == 1 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "shared futex wakes a waiter in another process");
    munmap((void *)sh, 4096);
}

/* ---- signals and threads ------------------------------------------------------ */
static volatile pid_t handled_by;
static volatile int handled_count;
static void on_usr(int sig) { (void)sig; handled_by = gettid_(); handled_count++; }

static volatile int stop_spin;
static volatile pid_t spinner_tid;
static void *spinner(void *arg)
{
    (void)arg;
    spinner_tid = gettid_();
    while (!stop_spin) sleep_ms(5);
    return NULL;
}

static void *blocker(void *arg)
{
    (void)arg;
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &s, NULL);               /* this thread will not take SIGUSR2 */
    while (!stop_spin) sleep_ms(5);
    return NULL;
}

static void test_signals(void)
{
    printf("signals:\n");
    signal(SIGUSR1, on_usr);
    signal(SIGUSR2, on_usr);
    pthread_t sp;
    stop_spin = 0;
    pthread_create(&sp, NULL, spinner, NULL);
    while (!spinner_tid) sched_yield();
    handled_by = 0;
    CHECK(syscall(SYS_tgkill, getpid(), spinner_tid, SIGUSR1) == 0, "tgkill to a thread");
    for (int i = 0; i < 200 && !handled_by; i++) sleep_ms(5);
    CHECK(handled_by == spinner_tid, "...is handled by that thread");
    errno = 0;
    CHECK(syscall(SYS_tgkill, getpid() + 100000, spinner_tid, SIGUSR1) == -1 && errno == ESRCH,
          "tgkill with the wrong tgid: ESRCH");
    CHECK(pthread_kill(sp, 0) == 0, "pthread_kill(t, 0) of a live thread");

    /* process-directed: main blocks it, so the other thread must take it */
    sigset_t s, old;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &s, &old);
    handled_by = 0;
    kill(getpid(), SIGUSR2);
    for (int i = 0; i < 200 && !handled_by; i++) sleep_ms(5);
    CHECK(handled_by == spinner_tid, "kill() is taken by a thread that does not block it");
    stop_spin = 1;
    pthread_join(sp, NULL);

    /* blocked everywhere: stays pending for the process until unblocked */
    stop_spin = 0;
    pthread_t bl;
    pthread_create(&bl, NULL, blocker, NULL);
    sleep_ms(20);
    handled_by = 0;
    kill(getpid(), SIGUSR2);
    sleep_ms(30);
    sigset_t pend;
    sigpending(&pend);
    CHECK(handled_by == 0 && sigismember(&pend, SIGUSR2), "blocked in every thread: pending on the process");
    pthread_sigmask(SIG_SETMASK, &old, NULL);           /* main takes it now */
    CHECK(handled_by == gettid_(), "...delivered when a thread unblocks it");
    stop_spin = 1;
    pthread_join(bl, NULL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

/* ---- process lifetime: exit, exit_group, fatal signals, fork, exec ------------ */
static void *exit_in_thread(void *arg) { sleep_ms(20); exit((int)(intptr_t)arg); }
static volatile int *volatile null_ptr = (volatile int *)8;
static void *crash_in_thread(void *arg) { (void)arg; sleep_ms(20); *null_ptr = 1; return NULL; }
static int pipefd[2];
static void *last_one(void *arg) { (void)arg; sleep_ms(30); write(pipefd[1], "ok", 2); return NULL; }
static void *forker(void *arg)
{
    (void)arg;
    pid_t pid = fork();
    if (pid == 0) {
        char b[512] = { 0 };
        int fd = open("/proc/self/status", O_RDONLY);
        read(fd, b, sizeof b - 1);
        close(fd);
        char *t = strstr(b, "Threads:");
        _exit(t && atoi(t + 8) == 1 ? 7 : 1);
    }
    int st = -1;
    waitpid(pid, &st, 0);
    return (void *)(intptr_t)(WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}
static const char *self_path;
static char pid_arg[32];
static void *execer(void *arg) { (void)arg; execl(self_path, self_path, "exec-child", pid_arg, (char *)NULL); return NULL; }
static void *sleeper(void *arg) { (void)arg; for (;;) sleep_ms(100); return NULL; }

static int run_child(void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) { fn(); _exit(99); }
    int st = -1;
    if (waitpid(pid, &st, 0) != pid) return -1000;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
}
static void child_exit_group(void)
{
    pthread_t t, s;
    pthread_create(&s, NULL, sleeper, NULL);
    pthread_create(&t, NULL, exit_in_thread, (void *)(intptr_t)5);
    for (;;) pause();
}
static void child_crash(void)
{
    pthread_t t, s;
    pthread_create(&s, NULL, sleeper, NULL);
    pthread_create(&t, NULL, crash_in_thread, NULL);
    for (;;) pause();
}
/* Run in a freshly exec'd process: musl's fork() child does not register
 * the exit futex of its main thread (Linux's fork does not inherit it), so
 * a forked child's main thread cannot pthread_exit while others run. */
static void child_main_exits_first(void)
{
    char fd[16];
    snprintf(fd, sizeof fd, "%d", pipefd[1]);
    execl(self_path, self_path, "main-exits", fd, (char *)NULL);
}
static void child_exec(void)
{
    snprintf(pid_arg, sizeof pid_arg, "%d", getpid());
    pthread_t t, s;
    pthread_create(&s, NULL, sleeper, NULL);
    pthread_create(&t, NULL, execer, NULL);
    for (;;) pause();
}

static void test_lifetime(void)
{
    printf("process lifetime:\n");
    CHECK(run_child(child_exit_group) == 5, "exit() in a thread ends the whole process with its status");
    CHECK(run_child(child_crash) == -SIGSEGV, "a fault in one thread kills the process (SIGSEGV)");
    pipe(pipefd);
    int r = run_child(child_main_exits_first);
    char b[4] = { 0 };
    read(pipefd[0], b, 2);
    close(pipefd[0]);
    close(pipefd[1]);
    CHECK(r == 0 && !strcmp(b, "ok"), "pthread_exit in main: the other thread finishes the process");
    pthread_t f;
    void *fr;
    pthread_create(&f, NULL, forker, NULL);
    pthread_join(f, &fr);
    CHECK(fr == (void *)7, "fork() in a thread: the child has one thread");
    CHECK(run_child(child_exec) == 0, "execve() in a thread: other threads end, the pid stays");
}

/* ---- robust mutexes: the kernel marks a dead owner's lock ---------------------- */
static void test_robust(void)
{
    printf("robust futexes:\n");
    pthread_mutex_t *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m, &a);
    pid_t pid = fork();
    if (pid == 0) {
        pthread_mutex_lock(m);
        _exit(0);                                       /* exit_group: no unlock, no user cleanup */
    }
    int st;
    waitpid(pid, &st, 0);
    int r = pthread_mutex_lock(m);
    CHECK(r == EOWNERDEAD, "lock held by a dead process: EOWNERDEAD");
    if (r == EOWNERDEAD) {
        CHECK(pthread_mutex_consistent(m) == 0, "pthread_mutex_consistent");
        pthread_mutex_unlock(m);
    }
    CHECK(pthread_mutex_lock(m) == 0, "usable again");
    pthread_mutex_unlock(m);
    munmap(m, 4096);
}

/* ---- many threads -------------------------------------------------------------- */
static volatile int alive_now, alive_max;
static void *brief(void *arg)
{
    (void)arg;
    int n = __atomic_add_fetch(&alive_now, 1, __ATOMIC_SEQ_CST);
    int m;
    while ((m = alive_max) < n && !__atomic_compare_exchange_n(&alive_max, &m, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    sleep_ms(10);
    __atomic_sub_fetch(&alive_now, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void test_many(void)
{
    printf("many threads:\n");
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int ok = 0;
    for (int round = 0; round < 10; round++) {          /* 400 threads in all: slots are reused */
        pthread_t th[40];
        int n = 0;
        for (int i = 0; i < 40; i++) if (pthread_create(&th[n], &at, brief, NULL) == 0) n++;
        for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
        ok += n;
    }
    CHECK(ok == 400, "400 threads created and joined (40 at a time)");
    CHECK(alive_max > 1, "they really ran at the same time");
    int detached = 0;
    for (int i = 0; i < 50; i++) {
        pthread_t t;
        if (pthread_create(&t, &at, brief, NULL) == 0 && pthread_detach(t) == 0) detached++;
    }
    while (alive_now) sleep_ms(10);
    sleep_ms(50);
    CHECK(detached == 50, "50 detached threads come and go");
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CHECK(sched_getaffinity(0, sizeof cs, &cs) == 0 && CPU_ISSET(0, &cs), "sched_getaffinity");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "exec-child"))    /* execve'd from a thread: same pid, alone */
        return getpid() == atoi(argv[2]) && gettid_() == getpid() ? 0 : 1;
    if (argc == 3 && !strcmp(argv[1], "main-exits")) {
        pipefd[1] = atoi(argv[2]);
        pthread_t t;
        pthread_create(&t, NULL, last_one, NULL);
        pthread_exit(NULL);                             /* the process lives on in the thread */
    }
    self_path = argv[0];
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("threadtest: pid %d\n", getpid());
    test_basic();
    test_sync();
    test_futex();
    test_signals();
    test_lifetime();
    test_robust();
    test_many();
    if (failures) printf("threadtest: %d check(s) FAILED\n", failures);
    else printf("threadtest: all checks passed\n");
    return failures ? 1 : 0;
}
