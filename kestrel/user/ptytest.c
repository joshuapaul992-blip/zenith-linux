/* user/ptytest.c -- pseudo-terminals on Kestrel (musl)
 *
 * posix_openpt/grantpt/unlockpt/ptsname, canonical editing and echo, output
 * processing, raw mode with VMIN/VTIME, EOF, window size, and a child in
 * its own session with the slave as controlling terminal: line I/O,
 * /dev/tty, Ctrl+C -> SIGINT, master close -> SIGHUP, slave close -> EIO.
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

/* Read from fd until `want` appears or ~10 s pass; returns what was read. */
static int read_until(int fd, char *buf, size_t cap, const char *want)
{
    size_t n = 0;
    buf[0] = 0;
    for (int tries = 0; tries < 1000 && !strstr(buf, want); tries++) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 10) <= 0) continue;
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
    }
    return (int)n;
}

static volatile sig_atomic_t got_int;
static void on_int(int s) { (void)s; got_int = 1; }

/* The program the child runs on the slave. */
static int child_main(void)
{
    struct sigaction sa;                                /* no SA_RESTART: the read must end */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);
    char line[128];
    int tty = open("/dev/tty", O_WRONLY);
    if (tty >= 0) { write(tty, "via-dev-tty\n", 12); close(tty); }
    printf("ready isatty=%d\n", isatty(0));
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        printf("got: %s\n", line);
        fflush(stdout);
    }
    if (got_int) { printf("SIGINT\n"); fflush(stdout); return 5; }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0, "posix_openpt, grantpt, unlockpt");
    char *name = ptsname(m);
    CHECK(name && !strncmp(name, "/dev/pts/", 9), "ptsname gives /dev/pts/N");
    int s = open(name, O_RDWR | O_NOCTTY);
    struct termios tio;
    CHECK(s >= 0 && isatty(s) && tcgetattr(s, &tio) == 0 && (tio.c_lflag & ICANON) && (tio.c_lflag & ECHO),
          "slave opens; isatty; default termios is canonical with echo");
    char *tn = ttyname(s);
    CHECK(tn && !strcmp(tn, name), "ttyname(slave) == ptsname(master)");

    char buf[256];
    write(m, "hello\n", 6);
    ssize_t n = read(s, buf, sizeof buf);
    CHECK(n == 6 && !memcmp(buf, "hello\n", 6), "master writes a line, slave reads it");
    n = read_until(m, buf, sizeof buf, "\r\n");
    CHECK(n == 7 && !memcmp(buf, "hello\r\n", 7), "echo comes back with ONLCR (\\r\\n)");

    write(m, "abc\177d\025xyz\r", 10);                 /* erase c; kill line; CR -> NL */
    n = read(s, buf, sizeof buf);
    CHECK(n == 4 && !memcmp(buf, "xyz\n", 4), "canonical editing: erase, kill, CR -> NL");
    read_until(m, buf, sizeof buf, "xyz\r\n");

    write(s, "out\n", 4);
    n = read_until(m, buf, sizeof buf, "\r\n");
    CHECK(n == 5 && !memcmp(buf, "out\r\n", 5), "slave output: \\n becomes \\r\\n");

    write(m, "\004", 1);
    n = read(s, buf, sizeof buf);
    CHECK(n == 0, "Ctrl+D on an empty line: read returns 0 (EOF)");

    struct termios raw = tio;
    cfmakeraw(&raw);
    tcsetattr(s, TCSANOW, &raw);
    write(m, "q", 1);
    n = read(s, buf, sizeof buf);
    struct pollfd pm = { m, POLLIN, 0 };
    CHECK(n == 1 && buf[0] == 'q' && poll(&pm, 1, 50) == 0, "raw mode: one byte at a time, no echo");
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;
    tcsetattr(s, TCSANOW, &raw);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    n = read(s, buf, sizeof buf);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(n == 0 && ms >= 150 && ms < 1000, "VMIN=0 VTIME=2: read times out after ~200 ms");
    tcsetattr(s, TCSANOW, &tio);

    struct winsize ws = { 30, 100, 0, 0 }, got;
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0 && ioctl(s, TIOCGWINSZ, &got) == 0 && got.ws_row == 30 && got.ws_col == 100,
          "TIOCSWINSZ on the master, TIOCGWINSZ on the slave");
    /* keep `s` open until the child has the slave: like Linux, a master
     * whose slave side was opened and closed again reads EIO */

    /* a child in its own session, the slave as its controlling terminal */
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open(name, O_RDWR);                    /* session leader: becomes the ctty */
        ioctl(fd, TIOCSCTTY, 0);
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
        close(m);
        char *av[] = { "ptytest", "child", NULL };
        execv("/boot/bin/ptytest", av);
        _exit(127);
    }
    n = read_until(m, buf, sizeof buf, "ready");
    close(s);
    CHECK(strstr(buf, "via-dev-tty"), "child: /dev/tty opens its controlling pty");
    CHECK(strstr(buf, "ready isatty=1") != NULL, "child: stdin is a terminal");
    write(m, "ping\r", 5);
    n = read_until(m, buf, sizeof buf, "got: ping");
    CHECK(strstr(buf, "got: ping") != NULL, "line through the pty to the child and its answer back");
    write(m, "\003", 1);                                /* Ctrl+C */
    n = read_until(m, buf, sizeof buf, "SIGINT");
    int st = -1;
    waitpid(pid, &st, 0);
    CHECK(strstr(buf, "SIGINT") && WIFEXITED(st) && WEXITSTATUS(st) == 5,
          "Ctrl+C sends SIGINT to the foreground group (read interrupted, child exits 5)");
    n = read(m, buf, sizeof buf);
    while (n > 0) n = read(m, buf, sizeof buf);
    CHECK(n == -1 && errno == EIO, "after the slave side closes, master read fails with EIO");
    close(m);

    /* hang-up */
    m = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(m); unlockpt(m);
    name = ptsname(m);
    int ready[2];
    pipe(ready);
    pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open(name, O_RDWR);
        ioctl(fd, TIOCSCTTY, 0);
        close(m);
        write(ready[1], "r", 1);                        /* the pty is our terminal now */
        for (;;) pause();
    }
    char r;
    read(ready[0], &r, 1);
    close(m);
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP, "closing the master sends SIGHUP to the session");

    printf(failures ? "ptytest: FAILED (%d)\n" : "ptytest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
