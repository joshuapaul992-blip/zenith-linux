/* include/kernel/usyscall.h -- user-side system-call stubs (a libc in miniature)
 *
 * These issue `int $0x80` exactly as a ring-3 program would, so code built
 * on them (init in kernel.c) only talks to the kernel through the syscall
 * ABI and can move to user mode unchanged once ELF loading lands. */
#ifndef KESTREL_USYSCALL_H
#define KESTREL_USYSCALL_H

#include <kernel/posix.h>
#include <kernel/syscall.h>

#define LINUX_REBOOT_MAGIC1         0xfee1deadu
#define LINUX_REBOOT_MAGIC2         672274793u
#define LINUX_REBOOT_CMD_RESTART    0x01234567u
#define LINUX_REBOOT_CMD_HALT       0xCDEF0123u
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321FEDCu

static inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "memory");
    return ret;
}
#define __sc0(n)             __syscall6(n, 0, 0, 0, 0, 0, 0)
#define __sc1(n,a)           __syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define __sc2(n,a,b)         __syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define __sc3(n,a,b,c)       __syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define __sc4(n,a,b,c,d)     __syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static inline ssize_t u_read(int fd, void *b, size_t n)          { return __sc3(SYS_read, fd, b, n); }
static inline ssize_t u_write(int fd, const void *b, size_t n)   { return __sc3(SYS_write, fd, b, n); }
static inline int     u_open(const char *p, int fl, mode_t m)    { return (int)__sc3(SYS_open, p, fl, m); }
static inline int     u_close(int fd)                            { return (int)__sc1(SYS_close, fd); }
static inline int     u_stat(const char *p, struct stat *st)     { return (int)__sc2(SYS_stat, p, st); }
static inline off_t   u_lseek(int fd, off_t o, int w)            { return __sc3(SYS_lseek, fd, o, w); }
static inline int     u_ioctl(int fd, unsigned long r, void *a)  { return (int)__sc3(SYS_ioctl, fd, r, a); }
static inline int     u_getdents64(int fd, void *b, size_t n)    { return (int)__sc3(SYS_getdents64, fd, b, n); }
static inline int     u_getcwd(char *b, size_t n)                { return (int)__sc2(SYS_getcwd, b, n); }
static inline int     u_chdir(const char *p)                     { return (int)__sc1(SYS_chdir, p); }
static inline int     u_mkdir(const char *p, mode_t m)           { return (int)__sc2(SYS_mkdir, p, m); }
static inline int     u_unlink(const char *p)                    { return (int)__sc1(SYS_unlink, p); }
static inline int     u_rmdir(const char *p)                     { return (int)__sc1(SYS_rmdir, p); }
static inline int     u_rename(const char *a, const char *b)     { return (int)__sc2(SYS_rename, a, b); }
static inline int     u_chmod(const char *p, mode_t m)           { return (int)__sc2(SYS_chmod, p, m); }
static inline int     u_chown(const char *p, int u, int g)       { return (int)__sc3(SYS_chown, p, u, g); }
static inline int     u_fstat(int fd, struct stat *st)           { return (int)__sc2(SYS_fstat, fd, st); }
static inline uid_t   u_getuid(void)                             { return (uid_t)__sc0(SYS_getuid); }
static inline gid_t   u_getgid(void)                             { return (gid_t)__sc0(SYS_getgid); }
/* utimensat(AT_FDCWD, path, NULL, 0): set the timestamps to now */
static inline int     u_touch_now(const char *p)                 { return (int)__sc4(SYS_utimensat, -100, p, 0, 0); }
static inline pid_t   u_getpid(void)                             { return (pid_t)__sc0(SYS_getpid); }
static inline pid_t   u_getppid(void)                            { return (pid_t)__sc0(SYS_getppid); }
static inline int     u_uname(struct utsname *u)                 { return (int)__sc1(SYS_uname, u); }
static inline int     u_sched_yield(void)                        { return (int)__sc0(SYS_sched_yield); }
static inline int     u_nanosleep(const struct timespec *r)      { return (int)__sc2(SYS_nanosleep, r, 0); }
static inline int     u_clock_gettime(int c, struct timespec *t) { return (int)__sc2(SYS_clock_gettime, c, t); }
static inline int     u_reboot(unsigned cmd)                     { return (int)__sc4(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, cmd, 0); }
static inline long    u_raw(long nr)                             { return __sc0(nr); }
__attribute__((noreturn)) static inline void u_exit(int code)    { __sc1(SYS_exit, code); for (;;) ; }

#endif
