/* user/faulttest.c -- hostile pointers and CPU faults must never hurt the kernel
 *
 * 1. System calls given bad pointers (NULL, kernel addresses, unmapped
 *    pages, buffers running off the end of the user half, strings without
 *    a terminator before an unmapped page) fail with EFAULT -- the kernel
 *    copies through its fault-tolerant uaccess path -- and leak nothing.
 * 2. CPU exceptions in user code become the Linux signals with the right
 *    si_code / si_addr, delivered to SA_SIGINFO handlers; unhandled (or
 *    blocked) ones end the process with that signal; a signal that cannot
 *    be delivered because the stack is bad ends it with SIGSEGV.
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
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

#define KERNEL_PTR ((void *)0x200000)                 /* the kernel image */
#define LOW_PTR    ((void *)0x1000)
#define END_PTR    ((void *)(0x800000000000ul - 4))   /* 4 bytes before the end of the user half */

static long sc(long n, long a, long b, long c) { return syscall(n, a, b, c); }
static int efault(long r) { return r == -1 && errno == EFAULT; }

static sigjmp_buf jb;
static volatile int got_sig, got_code;
static volatile unsigned long got_addr;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got_sig = sig;
    got_code = si->si_code;
    got_addr = (unsigned long)si->si_addr;
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

/* Run fn in a child; return its wait status. */
static int in_child(void (*fn)(void))
{
    pid_t p = fork();
    if (p == 0) {
        for (int s = 1; s < 32; s++) signal(s, SIG_DFL);   /* not the parent's longjmp handlers */
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return st;
}

static void child_null_deref(void) { volatile int *p = NULL; (void)*p; }
static void child_blocked_segv(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    sigprocmask(SIG_BLOCK, &s, NULL);
    volatile int *p = (int *)8;
    *p = 1;
}
static int depth(int n) { volatile char pad[512]; pad[0] = (char)n; return n ? depth(n + 1) + pad[0] : 0; }
static void child_stack_overflow(void) { depth(1); }
static void on_usr1(int s) { (void)s; }
static void child_bad_stack(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    long pid = getpid();
    /* kill(self, SIGUSR1) with a stack pointer the frame cannot go to */
    __asm__ volatile("mov $0x10, %%rsp\n\tmov $62, %%eax\n\tmov %0, %%rdi\n\tmov $10, %%esi\n\tsyscall\n\tud2"
                     :: "r"(pid) : "rax", "rdi", "rsi", "rcx", "r11", "memory");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("faulttest: pid %d\n", getpid());

    /* ---- 1. bad pointers into system calls ---------------------------- */
    int fd = open("/proc/version", O_RDONLY);
    CHECK(fd >= 0, "open /proc/version");
    CHECK(efault(sc(SYS_read, fd, 0, 16)), "read into NULL: EFAULT");
    CHECK(efault(sc(SYS_read, fd, (long)LOW_PTR, 16)), "read into an unmapped low page: EFAULT");
    CHECK(efault(sc(SYS_read, fd, (long)KERNEL_PTR, 16)), "read into kernel memory: EFAULT");
    CHECK(efault(sc(SYS_read, fd, (long)END_PTR, 16)), "read past the end of the user half: EFAULT");
    CHECK(efault(sc(SYS_write, 1, (long)KERNEL_PTR, 16)), "write from kernel memory: EFAULT");
    CHECK(efault(sc(SYS_open, 0, O_RDONLY, 0)), "open(NULL): EFAULT");
    CHECK(efault(sc(SYS_open, (long)KERNEL_PTR, O_RDONLY, 0)), "open(kernel address): EFAULT");

    /* a path running into an unmapped page without a NUL */
    char *two = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(two != MAP_FAILED && munmap(two + 4096, 4096) == 0, "map a page with an unmapped page after it");
    memset(two, 'a', 4096);
    CHECK(efault(sc(SYS_open, (long)(two + 4096 - 16), O_RDONLY, 0)), "unterminated path before an unmapped page: EFAULT");
    char longpath[400];
    memset(longpath, 'x', sizeof longpath - 1);
    longpath[0] = '/';
    longpath[sizeof longpath - 1] = 0;
    CHECK(sc(SYS_open, (long)longpath, O_RDONLY, 0) == -1 && errno == ENAMETOOLONG, "over-long path: ENAMETOOLONG");

    CHECK(efault(sc(SYS_stat, (long)"/proc/version", 0, 0)), "stat into NULL: EFAULT");
    CHECK(efault(sc(SYS_stat, (long)"/proc/version", (long)KERNEL_PTR, 0)), "stat into kernel memory: EFAULT");
    CHECK(efault(sc(SYS_clock_gettime, CLOCK_MONOTONIC, (long)KERNEL_PTR, 0)), "clock_gettime into kernel memory: EFAULT");
    CHECK(efault(sc(SYS_uname, (long)END_PTR, 0, 0)), "uname past the end: EFAULT");
    CHECK(efault(sc(SYS_nanosleep, (long)LOW_PTR, 0, 0)), "nanosleep(bad request): EFAULT");
    CHECK(efault(sc(SYS_poll, (long)KERNEL_PTR, 1, 0)), "poll(kernel pollfd): EFAULT");
    struct iovec bad = { KERNEL_PTR, 16 };
    CHECK(efault(sc(SYS_writev, 1, (long)&bad, 1)), "writev with a kernel iov_base: EFAULT");
    CHECK(efault(sc(SYS_ioctl, 0, TCGETS, (long)KERNEL_PTR)), "ioctl(TCGETS) into kernel memory: EFAULT");
    CHECK(efault(sc(SYS_ioctl, 0, TIOCGWINSZ, (long)LOW_PTR)), "ioctl(TIOCGWINSZ) to an unmapped page: EFAULT");
    CHECK(efault(sc(SYS_getdents64, fd, (long)KERNEL_PTR, 4096)), "getdents64 into kernel memory: EFAULT");
    CHECK(efault(syscall(SYS_rt_sigaction, SIGUSR2, (long)KERNEL_PTR, 0, 8)), "rt_sigaction from kernel memory: EFAULT");
    struct msghdr *kmsg = KERNEL_PTR;
    CHECK(efault(sc(SYS_sendmsg, 1, (long)kmsg, 0)), "sendmsg(kernel msghdr): EFAULT");

    int before = dup(0);
    close(before);
    CHECK(efault(sc(SYS_pipe, (long)KERNEL_PTR, 0, 0)), "pipe into kernel memory: EFAULT");
    int after = dup(0);
    close(after);
    CHECK(after == before, "...and no descriptors leaked");

    char *argv_bad[] = { "x", KERNEL_PTR, NULL };
    CHECK(efault(sc(SYS_execve, (long)"/bin/sh", (long)argv_bad, 0)), "execve with a kernel argv[1]: EFAULT");
    CHECK(efault(sc(SYS_execve, (long)KERNEL_PTR, 0, 0)), "execve(kernel path): EFAULT");
    char buf[64];
    CHECK(sc(SYS_read, fd, (long)buf, sizeof buf) > 0, "the descriptor still works afterwards");
    close(fd);

    /* ---- 2. CPU faults become signals ------------------------------------ */
    catch(SIGSEGV);
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) { volatile int *p = (int *)16; (void)*p; }
    if (got_sig != SIGSEGV || got_code != SEGV_MAPERR || got_addr != 16)
        printf("        got signal %d code %d addr %lx\n", got_sig, got_code, got_addr);
    CHECK(got_sig == SIGSEGV && got_code == SEGV_MAPERR && got_addr == 16, "load from address 16: SIGSEGV, SEGV_MAPERR, si_addr 16");
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) { volatile int *p = KERNEL_PTR; *p = 1; }
    CHECK(got_sig == SIGSEGV && got_code == SEGV_MAPERR && got_addr == 0x200000, "store to kernel memory: SIGSEGV, SEGV_MAPERR (as Linux)");

    catch(SIGFPE);
    got_sig = 0;
    volatile int zero = 0;
    if (!sigsetjmp(jb, 1)) { volatile int x = 10 / zero; (void)x; }
    CHECK(got_sig == SIGFPE && got_code == FPE_INTDIV, "integer division by zero: SIGFPE, FPE_INTDIV");

    catch(SIGILL);
    got_sig = 0;
    static volatile unsigned long ud_addr;
    if (!sigsetjmp(jb, 1)) { unsigned long a; __asm__ volatile("lea 0f(%%rip), %0\n\tmov %0, %1\n0: ud2" : "=&r"(a), "=m"(ud_addr)); }
    if (got_sig != SIGILL || got_addr != ud_addr) printf("        got signal %d code %d addr %lx, ud2 at %lx\n", got_sig, got_code, got_addr, ud_addr);
    CHECK(got_sig == SIGILL && got_code == ILL_ILLOPN && got_addr == ud_addr, "ud2: SIGILL, ILL_ILLOPN, si_addr = the instruction");

    catch(SIGTRAP);
    got_sig = 0;
    if (!sigsetjmp(jb, 1)) __asm__ volatile("int3");
    CHECK(got_sig == SIGTRAP, "int3: SIGTRAP");

    catch(SIGBUS);
    got_sig = 0;
    static char align[16] __attribute__((aligned(16)));
    if (!sigsetjmp(jb, 1)) {
        __asm__ volatile("pushf; orl $0x40000, (%%rsp); popf\n\t"   /* EFLAGS.AC */
                         "movl 1(%0), %%eax\n\t"
                         "pushf; andl $~0x40000, (%%rsp); popf" :: "r"(align) : "rax", "memory");
    }
    __asm__ volatile("pushf; andl $~0x40000, (%%rsp); popf" ::: "memory");
    if (got_sig == 0)
        printf("  skip  misaligned load with EFLAGS.AC: this CPU (or emulator) does not check alignment\n");
    else
        CHECK(got_sig == SIGBUS && got_code == BUS_ADRALN, "misaligned load with EFLAGS.AC: SIGBUS, BUS_ADRALN");

    int st = in_child(child_null_deref);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "unhandled NULL dereference: the child dies of SIGSEGV");
    st = in_child(child_blocked_segv);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "a blocked SIGSEGV from a fault still kills");
    st = in_child(child_stack_overflow);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "runaway recursion: SIGSEGV, not a kernel problem");
    st = in_child(child_bad_stack);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "signal with an unusable stack: SIGSEGV");

    printf(failures ? "faulttest: FAILED (%d)\n" : "faulttest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
