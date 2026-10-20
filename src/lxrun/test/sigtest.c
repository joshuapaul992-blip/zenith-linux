/*
 * sigtest.c -- raw (no-libc) signal test for lxrun.
 *
 * Installs a SIGUSR1 handler via rt_sigaction (with its own restorer),
 * raises it with kill(getpid, SIGUSR1), and checks the handler ran and
 * control returned past the kill. Exercises the full signal path:
 * rt_sigaction -> pending -> delivery frame -> handler -> rt_sigreturn.
 *
 * Build on Linux / WSL / a cross toolchain:
 *     gcc -static -nostdlib -no-pie -fno-stack-protector -o sigtest_exec sigtest.c
 *     gcc -static-pie -nostdlib -fno-stack-protector    -o sigtest_pie  sigtest.c
 * Run:
 *     lxrun -v sigtest_exec
 * Expect:
 *     sigtest: raising SIGUSR1
 *       handler: caught SIGUSR1
 *     sigtest: control returned past kill
 * and exit code 0.
 */

#define SYS_write        1
#define SYS_rt_sigaction 13
#define SYS_getpid       39
#define SYS_kill         62
#define SYS_exit_group   231

#define SIGUSR1     10
#define SA_RESTORER 0x04000000

static long sc3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc4(long n, long a, long b, long c, long d)
{
    long r; register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void out(const char *s)  { sc3(SYS_write, 1, (long)s, slen(s)); }

/* Signal return trampoline: rt_sigreturn (nr 15). */
void restorer(void);
__asm__(".text\n.globl restorer\nrestorer:\n  mov $15, %eax\n  syscall\n");

struct ksig {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
};

static volatile int caught = 0;
static void handler(int s) { (void)s; out("  handler: caught SIGUSR1\n"); caught = 1; }

void _start(void)
{
    struct ksig sa;
    sa.handler  = handler;
    sa.flags    = SA_RESTORER;
    sa.restorer = restorer;
    sa.mask     = 0;
    sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8 /* sigsetsize */);

    out("sigtest: raising SIGUSR1\n");
    long pid = sc3(SYS_getpid, 0, 0, 0);
    sc3(SYS_kill, pid, SIGUSR1, 0);
    out("sigtest: control returned past kill\n");

    sc3(SYS_exit_group, caught ? 0 : 1, 0, 0);
}
