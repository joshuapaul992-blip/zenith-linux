/* user/libctest.c -- first program linked against a real libc (musl,
 * static-PIE) on Kestrel. Exercises the parts of libc the X server leans
 * on: stdio, malloc (brk and mmap paths), strings, files and directories,
 * time, floating point formatting, qsort, errno, environment and atexit.
 * Exit status 0 = every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/random.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void bye(void) { printf("libctest: atexit handler ran, %d failure(s)\n", failures); }

int main(int argc, char **argv)
{
    printf("libctest: musl libc on Kestrel, argc %d, argv[0] %s\n", argc, argv[0]);
    atexit(bye);

    struct utsname u;
    CHECK(uname(&u) == 0 && u.sysname[0], "uname");
    printf("        %s %s %s\n", u.sysname, u.release, u.machine);

    /* malloc: small blocks (brk) and a big one (mmap) */
    char *small[64];
    int ok = 1;
    for (int i = 0; i < 64; i++) { small[i] = malloc(100 + i * 37); if (!small[i]) ok = 0; else memset(small[i], i, 100 + i * 37); }
    for (int i = 0; i < 64 && ok; i++) if (small[i][99] != (char)i) ok = 0;
    for (int i = 0; i < 64; i++) free(small[i]);
    CHECK(ok, "malloc/free of 64 small blocks");
    char *big = malloc(8 << 20);
    CHECK(big != NULL, "malloc(8 MiB)");
    if (big) { memset(big, 0xab, 8 << 20); CHECK((unsigned char)big[(8 << 20) - 1] == 0xab, "8 MiB block writable"); free(big); }
    char *r = realloc(NULL, 10);
    r = realloc(r, 100000);
    CHECK(r != NULL, "realloc growth");
    free(r);

    /* strings, formatting, floating point */
    char buf[128];
    snprintf(buf, sizeof buf, "%d %x %s %.3f %e", -42, 0xbeef, "str", 3.14159, 6.02e23);
    CHECK(!strcmp(buf, "-42 beef str 3.142 6.020000e+23"), "snprintf with integers and floats");
    printf("        \"%s\"\n", buf);
    CHECK(strtod("2.5e3", NULL) == 2500.0 && atoi("1234") == 1234, "strtod / atoi");
    int v[8] = { 5, 3, 9, 1, 7, 2, 8, 4 };
    qsort(v, 8, sizeof *v, cmp_int);
    CHECK(v[0] == 1 && v[7] == 9, "qsort");

    /* files: read a text file from the boot volume with stdio */
    FILE *f = fopen("/boot/etc/motd", "r");
    CHECK(f != NULL, "fopen /boot/etc/motd");
    if (f) {
        int lines = 0;
        while (fgets(buf, sizeof buf, f)) lines++;
        fclose(f);
        printf("        %d line(s)\n", lines);
        CHECK(lines > 0, "fgets reads lines");
    }
    errno = 0;
    CHECK(open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT, "open of a missing file sets errno = ENOENT");

    struct stat st;
    CHECK(stat("/boot/bin/libctest", &st) == 0 && st.st_size > 0, "stat of our own binary");

    /* directories */
    DIR *d = opendir("/boot/bin");
    int n = 0;
    if (d) { struct dirent *e; while ((e = readdir(d))) n++; closedir(d); }
    CHECK(d && n >= 2, "opendir/readdir /boot/bin");

    /* time */
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    printf("        now: %s UTC\n", buf);
    CHECK(tm.tm_year + 1900 >= 2024, "time() and gmtime_r");
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(30000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 25 && ms < 1000, "usleep(30 ms)");

    CHECK(getenv("PATH") && !strcmp(getenv("PATH"), "/bin"), "getenv(PATH)");
    CHECK(stat("/bin/libctest", &st) == 0 && st.st_size > 0, "/bin shows /boot/bin (bind)");
    CHECK(getpid() > 0, "getpid");
    CHECK(isatty(1) == 1 || isatty(1) == 0, "isatty does not crash");

    /* descriptors: the X server's event loop is pipes/sockets + poll + O_NONBLOCK */
    int pfd[2];
    CHECK(pipe2(pfd, O_CLOEXEC) == 0, "pipe2(O_CLOEXEC)");
    CHECK((fcntl(pfd[0], F_GETFD) & FD_CLOEXEC) != 0, "fcntl(F_GETFD) sees FD_CLOEXEC");
    struct pollfd pl = { pfd[0], POLLIN, 0 };
    CHECK(poll(&pl, 1, 0) == 0, "poll: empty pipe is not readable");
    clock_gettime(CLOCK_MONOTONIC, &a);
    CHECK(poll(&pl, 1, 50) == 0, "poll times out on an empty pipe");
    clock_gettime(CLOCK_MONOTONIC, &b);
    ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 45 && ms < 1000, "poll timeout of 50 ms is honoured");
    CHECK(write(pfd[1], "ping", 4) == 4, "write to pipe");
    CHECK(poll(&pl, 1, -1) == 1 && (pl.revents & POLLIN), "poll: readable after a write");
    {   /* select(2): IceWM's event loop */
        fd_set rs, ws;
        FD_ZERO(&rs); FD_ZERO(&ws);
        FD_SET(pfd[0], &rs); FD_SET(pfd[1], &ws);
        struct timeval tv = { 0, 0 };
        int n = select(pfd[1] + 1, &rs, &ws, NULL, &tv);
        CHECK(n == 2 && FD_ISSET(pfd[0], &rs) && FD_ISSET(pfd[1], &ws), "select: pipe readable and writable");
    }
    char pb[8] = { 0 };
    CHECK(read(pfd[0], pb, sizeof pb) == 4 && !memcmp(pb, "ping", 4), "read from pipe");
    CHECK(fcntl(pfd[0], F_SETFL, O_NONBLOCK) == 0 && (fcntl(pfd[0], F_GETFL) & O_NONBLOCK), "fcntl(F_SETFL, O_NONBLOCK)");
    errno = 0;
    CHECK(read(pfd[0], pb, sizeof pb) == -1 && errno == EAGAIN, "non-blocking read of empty pipe: EAGAIN");
    int dd = dup(pfd[1]);
    CHECK(dd > pfd[1] && write(dd, "x", 1) == 1 && read(pfd[0], pb, 1) == 1 && pb[0] == 'x', "dup shares the pipe");
    close(dd);
    close(pfd[1]);
    pl.events = POLLIN;
    CHECK(poll(&pl, 1, 0) == 1 && (pl.revents & POLLHUP) && read(pfd[0], pb, 1) == 0, "writer closed: POLLHUP and EOF");
    close(pfd[0]);
    unsigned char rnd[32] = { 0 };
    int nz = 0;
    CHECK(getrandom(rnd, sizeof rnd, 0) == 32, "getrandom");
    for (int i = 0; i < 32; i++) nz += rnd[i] != 0;
    CHECK(nz > 16, "getrandom bytes look random");
    CHECK(access("/boot/bin/libctest", R_OK) == 0 && access("/nope", F_OK) == -1, "access");

    printf(failures ? "libctest: FAILED\n" : "libctest: all checks passed\n");
    return failures ? 1 : 0;
}
