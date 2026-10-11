/* vdso/vdso.c -- the vDSO's functions (see include/kernel/vdso.h)
 *
 * Built freestanding as a position-independent shared object with no
 * relocations: everything is hidden except the exported functions, and the
 * data page is reached PC-relative (vvar, placed by vdso.lds one page
 * before the image). Anything the data page cannot answer is a system
 * call, so the results are always the kernel's. */
#define VDSO_BUILD
#include <stdint.h>
#include <kernel/vdso.h>

struct timespec { int64_t tv_sec; int64_t tv_nsec; };
struct timeval  { int64_t tv_sec; int64_t tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };

#define SYS_gettimeofday    96
#define SYS_time            201
#define SYS_clock_gettime   228
#define SYS_clock_getres    229
#define SYS_getcpu          309
#define EINVAL              22

extern const struct vdso_data vvar __attribute__((visibility("hidden")));

#define EXPORT __attribute__((visibility("default")))

static inline long sys2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

static inline long sys3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t rdtsc_ordered(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Monotonic nanoseconds and the boot epoch, as kernel/time.c computes
 * them; 0 if the kernel has to be asked. */
static int read_clock(uint64_t *ns, int64_t *epoch)
{
    const volatile struct vdso_data *d = &vvar;
    for (;;) {
        uint32_t seq = d->seq;
        __asm__ volatile("" ::: "memory");
        if (seq & 1) continue;                          /* being updated */
        if (d->clock_mode != VDSO_CLOCK_TSC) return 0;
        uint64_t hz = d->tsc_hz, t = rdtsc_ordered() - d->tsc_start;
        int64_t e = d->boot_epoch;
        __asm__ volatile("" ::: "memory");
        if (d->seq != seq) continue;
        *ns = (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;
        *epoch = e;
        return 1;
    }
}

/* Linux clock ids: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME 2,
 * THREAD_CPUTIME 3, MONOTONIC_RAW 4, REALTIME_COARSE 5, MONOTONIC_COARSE 6,
 * BOOTTIME 7 (kernel/syscall.c: clock_kind). */
EXPORT int __vdso_clock_gettime(int clk, struct timespec *ts)
{
    uint64_t ns;
    int64_t epoch;
    int realtime = clk == 0 || clk == 5;
    int mono = clk == 1 || clk == 4 || clk == 6 || clk == 7;
    if ((realtime || mono) && read_clock(&ns, &epoch)) {
        ts->tv_sec = (int64_t)(ns / 1000000000ull) + (realtime ? epoch : 0);
        ts->tv_nsec = (int64_t)(ns % 1000000000ull);
        return 0;
    }
    return (int)sys2(SYS_clock_gettime, clk, (long)ts);
}

EXPORT int __vdso_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    uint64_t ns;
    int64_t epoch;
    if (!read_clock(&ns, &epoch)) return (int)sys2(SYS_gettimeofday, (long)tv, (long)tz);
    if (tv) {
        tv->tv_sec = (int64_t)(ns / 1000000000ull) + epoch;
        tv->tv_usec = (int64_t)(ns % 1000000000ull / 1000);
    }
    if (tz) { tz->tz_minuteswest = 0; tz->tz_dsttime = 0; }
    return 0;
}

EXPORT int64_t __vdso_time(int64_t *t)
{
    uint64_t ns;
    int64_t epoch;
    if (!read_clock(&ns, &epoch)) return sys2(SYS_time, (long)t, 0);
    int64_t s = (int64_t)(ns / 1000000000ull) + epoch;
    if (t) *t = s;
    return s;
}

EXPORT int __vdso_clock_getres(int clk, struct timespec *ts)
{
    if (clk < 0 || clk > 7) return -EINVAL;             /* as the kernel: 1 ns for every clock */
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = 1; }
    return 0;
}

EXPORT long __vdso_getcpu(unsigned *cpu, unsigned *node, void *cache)
{
    (void)cache;
    uint32_t aux;
    switch (vvar.cpu_mode) {
    case VDSO_CPU_RDPID: {
        uint64_t v;
        __asm__ volatile("rdpid %0" : "=r"(v));
        aux = (uint32_t)v;
        break;
    }
    case VDSO_CPU_RDTSCP: {
        uint32_t lo, hi;
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
        break;
    }
    default:
        return sys3(SYS_getcpu, (long)cpu, (long)node, 0);
    }
    if (cpu) *cpu = aux & 0xfff;
    if (node) *node = aux >> 12;
    return 0;
}

int clock_gettime(int, struct timespec *) EXPORT __attribute__((weak, alias("__vdso_clock_gettime")));
int gettimeofday(struct timeval *, struct timezone *) EXPORT __attribute__((weak, alias("__vdso_gettimeofday")));
int64_t time(int64_t *) EXPORT __attribute__((weak, alias("__vdso_time")));
int clock_getres(int, struct timespec *) EXPORT __attribute__((weak, alias("__vdso_clock_getres")));
long getcpu(unsigned *, unsigned *, void *) EXPORT __attribute__((weak, alias("__vdso_getcpu")));
