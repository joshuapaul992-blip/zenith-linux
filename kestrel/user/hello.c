/* user/hello.c -- first ring-3 program for Kestrel, and a self-test of the
 * process ABI a static musl binary (and later the X server) relies on:
 * the System V initial stack, ring 3, brk, anonymous mmap/munmap, TLS via
 * arch_prctl(ARCH_SET_FS), writev, SSE state and clock_gettime.
 *
 * Freestanding on purpose (no libc yet): built with -nostdlib -static-pie
 * and written without absolute pointers in data, so it needs no
 * relocations. `hello crash` writes to kernel memory to check that the
 * kernel kills the process instead of crashing. */
#include <stdint.h>
#include <stddef.h>

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc(1, 1, (long)s, (long)slen(s), 0, 0, 0); }
static void outn(uint64_t v, int base)
{
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v % (uint64_t)base]; v /= (uint64_t)base; } while (v);
    out(b + i);
}
static int failures;
static void check(int ok, const char *what)
{
    out(ok ? "  ok    " : "  FAIL  "); out(what); out("\n");
    if (!ok) failures++;
}

struct iovec { const void *base; size_t len; };
struct timespec { int64_t sec, nsec; };

int cmain(uint64_t *sp)
{
    long argc = (long)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    uint64_t *aux = (uint64_t *)envp;
    while (*aux) aux++;
    aux++;
    uint64_t at_random = 0, at_pagesz = 0, at_phdr = 0;
    for (; aux[0]; aux += 2) {
        if (aux[0] == 25) at_random = aux[1];
        if (aux[0] == 6)  at_pagesz = aux[1];
        if (aux[0] == 3)  at_phdr = aux[1];
    }

    out("hello from ring 3 on Kestrel: pid ");
    outn((uint64_t)sc(39, 0, 0, 0, 0, 0, 0), 10);
    out(", argc "); outn((uint64_t)argc, 10);
    for (long i = 0; i < argc; i++) { out(" ["); out(argv[i]); out("]"); }
    out("\n");

    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    check((cs & 3) == 3, "running in ring 3 (CS RPL 3)");
    check(envp[0] && envp[0][0] == 'P', "environment passed (PATH=...)");
    check(at_pagesz == 4096 && at_random && at_phdr, "auxv: AT_PAGESZ, AT_RANDOM, AT_PHDR");
    check(((uint64_t)sp & 15) == 0, "stack 16-byte aligned at entry");

    /* brk: grow by 1 MiB, touch every page */
    uint64_t b0 = (uint64_t)sc(12, 0, 0, 0, 0, 0, 0);
    uint64_t b1 = (uint64_t)sc(12, (long)(b0 + (1 << 20)), 0, 0, 0, 0, 0);
    int ok = b1 == b0 + (1 << 20);
    for (uint64_t a = b0; ok && a < b1; a += 4096) { *(volatile uint32_t *)a = (uint32_t)a; ok = *(volatile uint32_t *)a == (uint32_t)a; }
    check(ok, "brk grows by 1 MiB and the memory is usable");

    /* anonymous mmap: zero-filled, writable, and can be unmapped */
    uint8_t *m = (uint8_t *)sc(9, 0, 256 * 1024, 3, 0x22, -1, 0);
    ok = (long)m > 0;
    for (int i = 0; ok && i < 256 * 1024; i += 512) ok = m[i] == 0;
    if (ok) { m[100] = 0x5a; ok = m[100] == 0x5a; }
    check(ok, "mmap(MAP_ANONYMOUS) 256 KiB, zero-filled and writable");
    check(sc(11, (long)m, 256 * 1024, 0, 0, 0, 0) == 0, "munmap");

    /* TLS: musl points FS at a thread block whose first word is its own address */
    static uint64_t tls[8];
    tls[0] = (uint64_t)&tls[0];
    tls[1] = 0x1234abcdull;
    check(sc(158, 0x1002, (long)tls, 0, 0, 0, 0) == 0, "arch_prctl(ARCH_SET_FS)");
    uint64_t self, val;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(self));
    __asm__ volatile("mov %%fs:8, %0" : "=r"(val));
    check(self == (uint64_t)tls && val == 0x1234abcdull, "%fs-relative loads see the TLS block");

    /* writev, as musl's stdio uses it */
    struct iovec iov[2];
    iov[0].base = "  ..    writev part one, "; iov[0].len = 26;
    iov[1].base = "part two\n";                iov[1].len = 9;
    check(sc(20, 1, (long)iov, 2, 0, 0, 0) == 35, "writev of two buffers");

    /* SSE state survives a few context switches (sched_yield) */
    uint64_t in = 0x0123456789abcdefull, res;
    __asm__ volatile("movq %0, %%xmm7" :: "r"(in) : "xmm7");
    for (int i = 0; i < 50; i++) sc(24, 0, 0, 0, 0, 0, 0);
    __asm__ volatile("movq %%xmm7, %0" : "=r"(res));
    check(res == in, "SSE register preserved across 50 sched_yield()s");

    struct timespec t0, t1;
    sc(228, 1, (long)&t0, 0, 0, 0, 0);
    struct timespec nap = { 0, 20 * 1000 * 1000 };
    sc(35, (long)&nap, 0, 0, 0, 0, 0);
    sc(228, 1, (long)&t1, 0, 0, 0, 0);
    int64_t ms = (t1.sec - t0.sec) * 1000 + (t1.nsec - t0.nsec) / 1000000;
    check(ms >= 15 && ms < 1000, "nanosleep(20 ms) measured by clock_gettime");

    /* the kernel must refuse kernel addresses from user code */
    check(sc(1, 1, 0x100000, 16, 0, 0, 0) == -14, "write() of a kernel address returns -EFAULT");

    if (argc > 1 && argv[1][0] == 'c') {
        out("  ..    writing to kernel memory at 0x1000, expect: killed (signal 11)\n");
        *(volatile uint64_t *)0x1000 = 1;
        out("  FAIL  survived a write to kernel memory\n");
        return 99;
    }
    out(failures ? "hello: FAILED\n" : "hello: all checks passed\n");
    return failures ? 1 : 42;
}

__asm__(".globl _start\n_start:\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call cmain\n"
        "  mov %eax, %edi\n"
        "  mov $60, %eax\n"
        "  syscall\n"
        "  hlt\n");
