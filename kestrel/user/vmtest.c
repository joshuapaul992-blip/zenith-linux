/* user/vmtest.c -- virtual memory: demand paging, copy-on-write, NX,
 * mprotect, file and shared mappings, memfd, mremap, madvise, OOM.
 * Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile uintptr_t got_addr;
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got_sig = sig; got_code = si->si_code; got_addr = (uintptr_t)si->si_addr;
    siglongjmp(jb, 1);
}
static void catch(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(sig, &sa, NULL);
}

/* Did touching p (read or write) fault? Returns the signal (0 = none). */
static int touch(volatile char *p, int write)
{
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) { if (write) *p = 1; else (void)*p; }
    return got_sig;
}

static long rss_kib(void)
{
    char b[256] = { 0 };
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return -1;
    read(fd, b, sizeof b - 1);
    close(fd);
    char *v = strstr(b, "VmRSS:");
    return v ? atol(v + 6) : -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("vmtest: pid %d\n", getpid());
    catch(SIGSEGV);
    catch(SIGBUS);
    const long pg = 4096;

    /* ---- demand paging ------------------------------------------------- */
    long r0 = rss_kib();
    char *big = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(big != MAP_FAILED, "mmap 256 MiB of anonymous memory");
    long r1 = rss_kib();
    CHECK(r1 >= 0 && r1 - r0 < 1024, "...costs nothing until touched (demand paging)");
    for (int i = 0; i < 64; i++) big[(size_t)i * (4u << 20)] = (char)i;
    long r2 = rss_kib();
    CHECK(r2 - r1 >= 64 * 4 && r2 - r1 < 64 * 4 + 512, "touching 64 pages makes ~64 pages resident");
    int zero = 1;
    for (int i = 1; i < 4096; i++) if (big[(256u << 20) - pg + (size_t)i]) zero = 0;
    CHECK(zero, "untouched anonymous memory reads as zeros");
    munmap(big, 256u << 20);

    /* ---- copy-on-write ------------------------------------------------- */
    size_t n = 8u << 20;
    unsigned char *buf = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i * 7);
    int pipefd[2];
    pipe(pipefd);
    pid_t c = fork();
    if (c == 0) {
        int ok = 1;
        for (size_t i = 0; i < n; i++) if (buf[i] != (unsigned char)(i * 7)) { ok = 0; break; }
        for (size_t i = 0; i < n; i += 4096) buf[i] = 0xAA;   /* child's private copies */
        char x;
        read(pipefd[0], &x, 1);                                  /* wait until the parent wrote */
        for (size_t i = 1; i < n; i += 4096) if (buf[i] != (unsigned char)(i * 7)) ok = 0;
        _exit(ok ? 0 : 1);
    }
    for (size_t i = 1; i < n; i += 4096) buf[i] = 0x55;            /* parent writes after fork */
    write(pipefd[1], "x", 1);
    int st;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork: the child sees the parent's data, its writes stay private");
    int pok = 1;
    for (size_t i = 0; i < n; i += 4096) if (buf[i] != (unsigned char)(i * 7)) pok = 0;
    CHECK(pok, "...and the child's writes never reach the parent (copy-on-write)");
    int forks_ok = 1;
    for (int k = 0; k < 4; k++) {
        pid_t p = fork();
        if (p == 0) { memset(buf, k, n); for (size_t i = 0; i < n; i++) if (buf[i] != k) _exit(1); _exit(0); }
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) forks_ok = 0;
    }
    CHECK(forks_ok && buf[12345] == (unsigned char)(12345 * 7), "4 children rewrite 8 MiB each; the parent's copy is intact");

    /* ---- protection ------------------------------------------------------ */
    char *p = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p, 'x', 3 * pg);
    CHECK(mprotect(p + pg, pg, PROT_READ) == 0, "mprotect the middle page read-only");
    CHECK(touch(p + pg, 1) == SIGSEGV && got_code == SEGV_ACCERR && got_addr == (uintptr_t)(p + pg),
          "writing it: SIGSEGV, SEGV_ACCERR at that address");
    CHECK(touch(p + pg, 0) == 0 && p[pg] == 'x', "reading it still works, the data is kept");
    CHECK(touch(p, 1) == 0 && touch(p + 2 * pg, 1) == 0, "its neighbours stay writable (region split)");
    CHECK(mprotect(p, 3 * pg, PROT_NONE) == 0 && touch(p, 0) == SIGSEGV, "PROT_NONE: even reading faults");
    CHECK(mprotect(p, 3 * pg, PROT_READ | PROT_WRITE) == 0 && touch(p + pg, 1) == 0 && p[2] == 'x',
          "back to read/write: data intact");
    CHECK(syscall(SYS_mprotect, p + 1, pg, PROT_READ) == -1 && errno == EINVAL, "mprotect of an unaligned address: EINVAL");
    CHECK(syscall(SYS_mprotect, p, pg, 0x10) == -1 && errno == EINVAL, "mprotect with unknown bits: EINVAL");

    /* NX: no executing data */
    static const unsigned char ret_42[] = { 0xb8, 42, 0, 0, 0, 0xc3 };    /* mov eax, 42; ret */
    memcpy(p, ret_42, sizeof ret_42);
    int (*fn)(void) = (int (*)(void))p;
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) fn();
    CHECK(got_sig == SIGSEGV && got_addr == (uintptr_t)p, "jumping into a data page: SIGSEGV (NX)");
    char stackcode[16];
    memcpy(stackcode, ret_42, sizeof ret_42);
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) ((int (*)(void))stackcode)();
    CHECK(got_sig == SIGSEGV, "executing the stack: SIGSEGV (NX)");
    CHECK(mprotect(p, pg, PROT_READ | PROT_EXEC) == 0 && fn() == 42, "mprotect(PROT_READ|PROT_EXEC): the code runs (JIT-style)");
    CHECK(touch(p, 1) == SIGSEGV, "...and the page is no longer writable (W^X)");

    /* munmap a hole */
    CHECK(munmap(p + pg, pg) == 0 && touch(p + pg, 0) == SIGSEGV && got_code == SEGV_MAPERR,
          "munmap the middle page: SIGSEGV, SEGV_MAPERR there");
    CHECK(touch(p + 2 * pg, 0) == 0, "...the pages around it are still mapped");
    munmap(p, 3 * pg);

    /* ---- file mappings ---------------------------------------------------- */
    int fd = open("/tmp/vmtest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    char line[64];
    for (int i = 0; i < 1000; i++) { snprintf(line, sizeof line, "line %04d\n", i); write(fd, line, 10); }
    char *fp = mmap(NULL, 10000, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(fp != MAP_FAILED && !memcmp(fp + 5000, "line 0500\n", 10), "MAP_PRIVATE of a file: its bytes");
    fp[0] = 'L';
    char b5[5];
    pread(fd, b5, 5, 0);
    CHECK(!memcmp(b5, "line ", 5), "...writes to a private mapping do not reach the file");
    CHECK(touch(fp + 10000 + 100, 0) == 0 && fp[10000 + 100] == 0, "past EOF in the last page: zeros");
    char *fp2 = mmap(NULL, 4 * pg, PROT_READ, MAP_PRIVATE, fd, 0);    /* the file is 10000 bytes */
    CHECK(touch(fp2 + 3 * pg + 10, 0) == SIGBUS, "a page entirely past EOF: SIGBUS");
    munmap(fp2, 4 * pg);
    munmap(fp, 10000);

    char *sp = mmap(NULL, 10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(sp != MAP_FAILED, "MAP_SHARED of a /tmp file");
    memcpy(sp + 10, "SHARED", 6);
    char b6[6];
    pread(fd, b6, 6, 10);
    CHECK(!memcmp(b6, "SHARED", 6), "...a store through the mapping is what read() returns");
    pwrite(fd, "PWRITE", 6, 20);
    CHECK(!memcmp(sp + 20, "PWRITE", 6), "...and write() shows up in the mapping");
    c = fork();
    if (c == 0) { memcpy(sp + 30, "CHILD!", 6); _exit(0); }
    waitpid(c, &st, 0);
    CHECK(!memcmp(sp + 30, "CHILD!", 6), "...a child's store through its copy of the mapping reaches the parent");
    munmap(sp, 10000);
    unlink("/tmp/vmtest.dat");
    char b4[4];
    CHECK(pread(fd, b4, 4, 0) == 4, "an unlinked file stays readable through its descriptor");
    close(fd);

    /* shared anonymous memory and memfd across fork */
    int *shared = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *shared = 1;
    c = fork();
    if (c == 0) { *shared = 42; _exit(0); }
    waitpid(c, &st, 0);
    CHECK(*shared == 42, "MAP_SHARED|MAP_ANONYMOUS: the child's store is seen by the parent");
    int mfd = syscall(SYS_memfd_create, "vmtest", 1 /* MFD_CLOEXEC */);
    CHECK(mfd >= 0 && ftruncate(mfd, 2 * pg) == 0, "memfd_create + ftruncate");
    char *m1 = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    char *m2 = mmap(NULL, 2 * pg, PROT_READ, MAP_SHARED, mfd, 0);
    strcpy(m1 + pg, "via m1");
    CHECK(m1 != MAP_FAILED && m2 != MAP_FAILED && !strcmp(m2 + pg, "via m1"), "two mappings of one memfd share pages");
    struct stat mst;
    fstat(mfd, &mst);
    CHECK(mst.st_size == 2 * pg, "fstat sees the memfd's size");
    close(mfd);

    /* ---- mremap, madvise, brk, maps --------------------------------------- */
    char *r = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(r, 'r', 4 * pg);
    mmap(r + 4 * pg, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);   /* block growth in place */
    char *rm = mremap(r, 4 * pg, 64 * pg, MREMAP_MAYMOVE);
    CHECK(rm != MAP_FAILED && rm != r && rm[4 * pg - 1] == 'r' && rm[0] == 'r', "mremap moves the region, contents with it");
    CHECK(touch(rm + 63 * pg, 1) == 0, "...the grown part is usable");
    CHECK(touch(r, 0) == SIGSEGV, "...the old address is gone");
    memset(rm, 7, pg);
    CHECK(madvise(rm, pg, MADV_DONTNEED) == 0 && rm[100] == 0, "madvise(DONTNEED): anonymous pages read as zeros again");
    char *brk0 = (char *)syscall(SYS_brk, 0);           /* musl's sbrk() only does sbrk(0) */
    CHECK((char *)syscall(SYS_brk, brk0 + (1 << 20)) == brk0 + (1 << 20) && touch(brk0 + (1 << 20) - 1, 1) == 0,
          "brk grows the heap by 1 MiB");
    CHECK((char *)syscall(SYS_brk, brk0) == brk0 && touch(brk0 + (1 << 20) - 1, 0) == SIGSEGV, "...and shrinks it again");
    char maps[4096] = { 0 };
    int mf = open("/proc/self/maps", O_RDONLY);
    read(mf, maps, sizeof maps - 1);
    close(mf);
    CHECK(strstr(maps, "[stack]") && strstr(maps, "r-xp") && strstr(maps, "/boot/bin/vmtest"), "/proc/self/maps lists the regions");

    /* ---- out of memory ----------------------------------------------------- */
    c = fork();
    if (c == 0) {
        for (;;) {                                      /* allocate and touch until the OOM killer acts */
            char *m = mmap(NULL, 16u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) continue;
            for (size_t i = 0; i < (16u << 20); i += 4096) m[i] = 1;
        }
    }
    waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "a process that eats all memory is killed by the OOM killer");
    char *after = mmap(NULL, 16u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(after, 1, 16u << 20);
    CHECK(after != MAP_FAILED && after[(16u << 20) - 1] == 1, "...and its memory came back");

    printf(failures ? "vmtest: FAILED (%d)\n" : "vmtest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
