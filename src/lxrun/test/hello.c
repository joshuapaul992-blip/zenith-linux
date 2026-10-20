/*
 * hello.c -- zero-dependency Linux x86-64 ELF smoke test for lxrun.
 *
 * No libc, no TLS, no brk: exercises only the loader, write(2), and
 * exit_group(2). The ideal first thing to run after building lxrun.
 *
 * Build on a Linux box (or in WSL / a cross toolchain):
 *     ET_EXEC (loads at 0x400000):
 *         gcc -static -nostdlib -no-pie -fno-stack-protector -o hello_exec hello.c
 *     static-PIE (ET_DYN, exercises the bias + self-reloc path):
 *         gcc -static-pie -nostdlib -fno-stack-protector -o hello_pie hello.c
 *
 * Then on Windows:
 *     lxrun -v hello_exec
 *     lxrun -v hello_pie
 * Expected output:
 *     hello from a Linux ELF running on Windows via lxrun
 * and process exit code 7.
 */

static long sys_write(long fd, const void *buf, long n)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1), "D"(fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

static void sys_exit_group(long code)
{
    __asm__ volatile("syscall" : : "a"(231), "D"(code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

void _start(void)
{
    const char *msg = "hello from a Linux ELF running on Windows via lxrun\n";
    sys_write(1, msg, slen(msg));
    sys_exit_group(7);
}
