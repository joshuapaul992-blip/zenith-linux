/* user/proctest.c -- processes and signals on Kestrel (musl)
 *
 * fork, execve, posix_spawn (clone CLONE_VM|CLONE_VFORK), vfork, wait4,
 * sessions, and signals: handlers, masks, sigsuspend, kill, SIGCHLD,
 * SIGPIPE, EINTR/SA_RESTART, default actions, FPU state across handlers.
 * Exit status 0 = every check passed. `proctest exec-child N` exits N (the
 * image that execve and posix_spawn start). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>

extern char **environ;
static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static volatile sig_atomic_t got_usr1, got_usr2, got_chld;
static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_usr2(int s) { (void)s; got_usr2++; }
static void on_chld(int s) { (void)s; got_chld++; }
static volatile double fp_in_handler;
static void on_fp(int s) { volatile double x = s; for (int i = 0; i < 10; i++) x = x * 1.5 + 0.25; fp_in_handler = x; }

static void handler(int sig, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static int wait_status(pid_t pid)
{
    int st = -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    return st;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "exec-child")) {
        printf("proctest: exec-child running as pid %d, argv[2]=%s, env KESTREL_TEST=%s\n", getpid(), argv[2],
               getenv("KESTREL_TEST") ? getenv("KESTREL_TEST") : "(unset)");
        return atoi(argv[2]);
    }
    printf("proctest: pid %d\n", getpid());

    /* ---- fork + wait4 ---- */
    static int shared = 1;
    pid_t pid = fork();
    if (pid == 0) { shared = 99; _exit(7); }
    int st = wait_status(pid);
    CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork: child exit status 7 through waitpid");
    CHECK(shared == 1, "fork: the child's writes do not reach the parent");
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "waitpid with no children: ECHILD");

    int pfd[2];
    pipe(pfd);
    pid = fork();
    if (pid == 0) { close(pfd[0]); write(pfd[1], "from child", 10); _exit(0); }
    close(pfd[1]);
    char buf[32] = { 0 };
    ssize_t n = read(pfd[0], buf, sizeof buf - 1);
    close(pfd[0]);
    wait_status(pid);
    CHECK(n == 10 && !strcmp(buf, "from child"), "fork: inherited pipe carries data");

    pid = fork();
    if (pid == 0) { struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, NULL); _exit(0); }
    CHECK(waitpid(pid, &st, WNOHANG) == 0, "WNOHANG returns 0 while the child runs");
    wait_status(pid);

    pid_t kids[16];
    for (int i = 0; i < 16; i++) if ((kids[i] = fork()) == 0) _exit(i);
    int okk = 1;
    for (int i = 0; i < 16; i++) { st = wait_status(kids[i]); if (!WIFEXITED(st) || WEXITSTATUS(st) != i) okk = 0; }
    CHECK(okk, "16 children at once, each status collected");

    /* ---- execve ---- */
    pid = fork();
    if (pid == 0) {
        char *av[] = { "proctest", "exec-child", "42", NULL };
        char *ev[] = { "KESTREL_TEST=execve", NULL };
        execve("/boot/bin/proctest", av, ev);
        _exit(100 + errno);
    }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "execve in a forked child (exit 42 from the new image)");

    char *av0[] = { "nope", NULL };
    CHECK(execve("/boot/bin/does-not-exist", av0, environ) == -1 && errno == ENOENT,
          "execve of a missing file: ENOENT, caller unharmed");

    /* ---- posix_spawn: clone(CLONE_VM | CLONE_VFORK) + execve ---- */
    setenv("KESTREL_TEST", "posix_spawn", 1);
    char *sv[] = { "proctest", "exec-child", "33", NULL };
    int rc = posix_spawn(&pid, "/boot/bin/proctest", NULL, NULL, sv, environ);
    st = rc == 0 ? wait_status(pid) : -1;
    CHECK(rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 33, "posix_spawn + waitpid (exit 33)");
    rc = posix_spawn(&pid, "/boot/bin/does-not-exist", NULL, NULL, sv, environ);
    if (rc == 0) wait_status(pid);
    CHECK(rc == ENOENT, "posix_spawn of a missing file reports ENOENT");

    /* ---- vfork ---- */
    volatile int vf = 0;
    pid = vfork();
    if (pid == 0) { vf = 5; _exit(3); }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3 && vf == 5, "vfork: child shares memory until _exit");

    /* ---- sessions ---- */
    pid = fork();
    if (pid == 0) {
        pid_t s = setsid();
        _exit(s == getpid() && getsid(0) == getpid() && getpgid(0) == getpid() ? 0 : 1);
    }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "setsid in a child: new session and group");

    /* ---- signals ---- */
    handler(SIGUSR1, on_usr1, 0);
    raise(SIGUSR1);
    CHECK(got_usr1 == 1, "raise(SIGUSR1) runs the handler");
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 2, "kill(getpid(), SIGUSR1) runs the handler");

    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    sigset_t pend;
    sigpending(&pend);
    CHECK(got_usr1 == 2 && sigismember(&pend, SIGUSR1), "a blocked signal stays pending");
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(got_usr1 == 3, "unblocking delivers it");

    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    CHECK(got_usr1 == 3, "SIG_IGN discards it");

    /* floats in flight across a signal, and floats inside the handler */
    double acc = 1.0;
    for (int i = 0; i < 10; i++) acc = acc * 1.25 + 0.5;
    handler(SIGUSR2, on_fp, 0);
    raise(SIGUSR2);
    for (int i = 0; i < 10; i++) acc = acc * 1.25 + 0.5;
    double expect = 1.0, inh = SIGUSR2;
    for (int i = 0; i < 20; i++) expect = expect * 1.25 + 0.5;
    for (int i = 0; i < 10; i++) inh = inh * 1.5 + 0.25;
    CHECK(acc == expect && fp_in_handler == inh, "FPU state survives a handler that uses floats");

    handler(SIGCHLD, on_chld, SA_RESTART);
    pid = fork();
    if (pid == 0) _exit(0);
    wait_status(pid);
    CHECK(got_chld >= 1, "SIGCHLD reaches the parent's handler");
    signal(SIGCHLD, SIG_DFL);

    pid = fork();
    if (pid == 0) { for (;;) pause(); }
    struct timespec ts = { 0, 50 * 1000000L };
    nanosleep(&ts, NULL);
    kill(pid, SIGTERM);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "kill(child, SIGTERM): child ends, WTERMSIG = SIGTERM");

    pid = fork();
    if (pid == 0) { for (;;) ; }                       /* busy in ring 3 */
    nanosleep(&ts, NULL);
    kill(pid, SIGKILL);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "SIGKILL ends a child spinning in user mode");

    /* EINTR vs SA_RESTART on a blocking read */
    pipe(pfd);
    pid_t me = getpid();
    handler(SIGUSR2, on_usr2, 0);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR2); nanosleep(&ts, NULL); write(pfd[1], "x", 1); _exit(0); }
    n = read(pfd[0], buf, 1);
    CHECK(n == -1 && errno == EINTR && got_usr2 == 1, "read interrupted by a handler without SA_RESTART: EINTR");
    read(pfd[0], buf, 1);
    wait_status(pid);
    handler(SIGUSR2, on_usr2, SA_RESTART);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR2); nanosleep(&ts, NULL); write(pfd[1], "y", 1); _exit(0); }
    n = read(pfd[0], buf, 1);
    CHECK(n == 1 && buf[0] == 'y' && got_usr2 == 2, "with SA_RESTART the read is restarted and gets the data");
    wait_status(pid);
    close(pfd[0]); close(pfd[1]);

    /* sigsuspend */
    sigprocmask(SIG_BLOCK, &set, &old);                 /* SIGUSR1 blocked */
    handler(SIGUSR1, on_usr1, 0);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR1); _exit(0); }
    sigset_t none;
    sigemptyset(&none);
    int r = sigsuspend(&none);
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    CHECK(r == -1 && errno == EINTR && got_usr1 == 4 && sigismember(&now, SIGUSR1),
          "sigsuspend waits for the signal and restores the mask");
    sigprocmask(SIG_SETMASK, &old, NULL);
    wait_status(pid);

    /* nanosleep interrupted */
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR1); _exit(0); }
    struct timespec longer = { 5, 0 }, rem = { 0, 0 };
    r = nanosleep(&longer, &rem);
    CHECK(r == -1 && errno == EINTR && rem.tv_sec >= 3, "nanosleep interrupted: EINTR and the time left");
    wait_status(pid);

    /* SIGPIPE */
    pipe(pfd);
    close(pfd[0]);
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(pfd[1], "z", 1) == -1 && errno == EPIPE, "write to a pipe without readers, SIGPIPE ignored: EPIPE");
    signal(SIGPIPE, SIG_DFL);
    pid = fork();
    if (pid == 0) { write(pfd[1], "z", 1); _exit(0); }
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "...with the default action the writer dies of SIGPIPE");
    close(pfd[1]);

    pid = fork();
    if (pid == 0) abort();
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "abort(): WTERMSIG = SIGABRT");

    printf(failures ? "proctest: FAILED (%d)\n" : "proctest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
