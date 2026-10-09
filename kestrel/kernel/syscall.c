/* kernel/syscall.c -- system-call table and handlers
 *
 * Implemented calls operate on the VFS and the scheduler. Calls that need
 * per-process address spaces (fork, execve, mmap, brk, ...) are wired to
 * placeholder handlers returning -ENOSYS so the vector table is complete and
 * each can be filled in independently. */
#include <kernel/syscall.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/bootinfo.h>
#include <kernel/time.h>

extern void syscall_entry(void);

static syscall_fn table[SYS_MAX];
static const char *names[SYS_MAX];
static uint64_t counts[SYS_MAX];

/* Minimal pointer validation. With per-process page tables this becomes
 * copy_from_user()/copy_to_user() with a fault-fixup table. */
static int bad_ptr(uint64_t p) { return p < 4096; }

static struct file **fd_slot(int fd)
{
    struct tcb *t = current_task();
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) return NULL;
    return &t->fds[fd];
}

/* ---- file I/O --------------------------------------------------------- */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (bad_ptr(buf)) return -EFAULT;
    return vfs_read(*f, (void *)buf, len);
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (bad_ptr(buf)) return -EFAULT;
    return vfs_write(*f, (const void *)buf, len);
}

static int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    struct tcb *t = current_task();
    int fd = 0;
    while (fd < MAX_FDS && t->fds[fd]) fd++;
    if (fd == MAX_FDS) return -EMFILE;
    struct file *f;
    int r = vfs_open((const char *)path, (int)flags, (mode_t)mode, &f);
    if (r < 0) return r;
    t->fds[fd] = f;
    return fd;
}

static int64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    vfs_close(*f);
    *f = NULL;
    return 0;
}

static int64_t sys_stat(uint64_t path, uint64_t st, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path) || bad_ptr(st)) return -EFAULT;
    return vfs_stat((const char *)path, (struct stat *)st);
}

static int64_t sys_fstat(uint64_t fd, uint64_t st, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (bad_ptr(st)) return -EFAULT;
    return vfs_fstat(*f, (struct stat *)st);
}

static int64_t sys_lseek(uint64_t fd, uint64_t off, uint64_t whence, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    return vfs_lseek(*f, (off_t)off, (int)whence);
}

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    return vfs_ioctl(*f, req, (void *)arg);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (bad_ptr(buf)) return -EFAULT;
    return vfs_getdents(*f, (void *)buf, len);
}

/* ---- paths -------------------------------------------------------------- */
static int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(buf)) return -EFAULT;
    int r = vfs_getcwd((char *)buf, size);
    return r < 0 ? r : (int64_t)strlen((char *)buf) + 1;
}

static int64_t sys_chdir(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    return vfs_chdir((const char *)path);
}

static int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    return vfs_mkdir((const char *)path, (mode_t)mode);
}

static int64_t sys_unlink(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    return vfs_unlink((const char *)path);
}

static int64_t sys_rmdir(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    return vfs_rmdir((const char *)path);
}

static int64_t sys_rename(uint64_t from, uint64_t to, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(from) || bad_ptr(to)) return -EFAULT;
    return vfs_rename((const char *)from, (const char *)to);
}

static int64_t sys_chmod(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    return vfs_chmod((const char *)path, (mode_t)mode);
}

static int64_t sys_chown(uint64_t path, uint64_t uid, uint64_t gid, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    if (current_task()->uid != 0) return -EPERM;        /* only root may give files away */
    return vfs_chown((const char *)path, (int)uid, (int)gid);
}

/* utimensat(dirfd, path, times[2], flags): AT_FDCWD (or absolute paths)
 * only; times NULL or UTIME_NOW = now. Only the modification time is kept. */
#define AT_FDCWD   -100
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
static int64_t sys_utimensat(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    if ((int)dirfd != AT_FDCWD && ((const char *)path)[0] != '/') return -ENOSYS;
    int64_t mtime = -1;
    if (times) {
        if (bad_ptr(times)) return -EFAULT;
        const struct timespec *ts = (const struct timespec *)times;
        if (ts[1].tv_nsec == UTIME_OMIT) return 0;
        if (ts[1].tv_nsec != UTIME_NOW) mtime = ts[1].tv_sec;
    }
    return vfs_utime((const char *)path, mtime);
}

/* ---- processes ---------------------------------------------------------- */
static int64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->pid; }

static int64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->ppid; }

static int64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->uid; }

static int64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->gid; }

static int64_t sys_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; sched_yield(); return 0; }

static int64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; task_exit((int)code); }

static int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)rem; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(req)) return -EFAULT;
    const struct timespec *ts = (const struct timespec *)req;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return -EINVAL;
    task_sleep_ms((uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000);
    return 0;
}

static int64_t sys_clock_gettime(uint64_t clk, uint64_t tsp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (clk != CLOCK_MONOTONIC && clk != CLOCK_REALTIME) return -EINVAL;
    if (bad_ptr(tsp)) return -EFAULT;
    struct timespec *ts = (struct timespec *)tsp;
    if (clk == CLOCK_REALTIME) { time_realtime(&ts->tv_sec, &ts->tv_nsec); return 0; }
    uint64_t ns = time_ns();                            /* HPET/TSC, nanosecond resolution */
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static int64_t sys_uname(uint64_t p, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(p)) return -EFAULT;
    struct utsname *u = (struct utsname *)p;
    memset(u, 0, sizeof *u);
    strlcpy(u->sysname, KESTREL_NAME, sizeof u->sysname);
    strlcpy(u->nodename, KESTREL_HOSTNAME, sizeof u->nodename);
    strlcpy(u->release, KESTREL_RELEASE, sizeof u->release);
    strlcpy(u->version, KESTREL_CODENAME, sizeof u->version);
    strlcpy(u->machine, KESTREL_MACHINE, sizeof u->machine);
    strlcpy(u->domainname, "(none)", sizeof u->domainname);
    return 0;
}

/* reboot(2): Linux-compatible magic numbers */
static int64_t sys_reboot(uint64_t m1, uint64_t m2, uint64_t cmd, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (m1 != 0xfee1deadu || m2 != 672274793u) return -EINVAL;
    kprintf("reboot: cmd=%lx\n", cmd);
    cli();
    switch (cmd) {
    case 0x01234567u:                                   /* RESTART: 8042 reset line */
        for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
        outb(0x64, 0xFE);
        __asm__ volatile("lidt %0; int3" :: "m"((struct { uint16_t l; uint64_t b; } __attribute__((packed))){0, 0}));
        break;
    case 0x4321FEDCu:                                   /* POWER_OFF: ACPI PM1a in common VMs */
        outw(0x604, 0x2000);                            /* QEMU (q35/pc, ACPI)  */
        outw(0xB004, 0x2000);                           /* Bochs, old QEMU      */
        outw(0x4004, 0x3400);                           /* VirtualBox           */
        /* real hardware needs ACPI AML parsing -> just halt */
        __attribute__((fallthrough));
    case 0xCDEF0123u:                                   /* HALT */
        panic("System halted. It is now safe to turn off your computer.");
    }
    return -EINVAL;
}

static int64_t sys_enosys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return -ENOSYS; }

/* ------------------------------------------------------------------------- */
#define REG(nr, fn) do { table[nr] = fn; names[nr] = #nr + 4; } while (0)

void syscall_init(void)
{
    REG(SYS_read, sys_read);           REG(SYS_write, sys_write);
    REG(SYS_open, sys_open);           REG(SYS_close, sys_close);
    REG(SYS_stat, sys_stat);           REG(SYS_fstat, sys_fstat);
    REG(SYS_lseek, sys_lseek);         REG(SYS_ioctl, sys_ioctl);
    REG(SYS_sched_yield, sys_sched_yield);
    REG(SYS_nanosleep, sys_nanosleep); REG(SYS_getpid, sys_getpid);
    REG(SYS_exit, sys_exit);           REG(SYS_exit_group, sys_exit);
    REG(SYS_uname, sys_uname);         REG(SYS_getcwd, sys_getcwd);
    REG(SYS_chdir, sys_chdir);         REG(SYS_mkdir, sys_mkdir);
    REG(SYS_rmdir, sys_rmdir);         REG(SYS_unlink, sys_unlink);
    REG(SYS_rename, sys_rename);       REG(SYS_chmod, sys_chmod);
    REG(SYS_chown, sys_chown);         REG(SYS_utimensat, sys_utimensat);
    REG(SYS_getuid, sys_getuid);       REG(SYS_getgid, sys_getgid);
    REG(SYS_getppid, sys_getppid);     REG(SYS_getdents64, sys_getdents64);
    REG(SYS_clock_gettime, sys_clock_gettime);
    REG(SYS_reboot, sys_reboot);
    /* placeholders: need user address spaces / signals */
    REG(SYS_mmap, sys_enosys);         REG(SYS_brk, sys_enosys);
    REG(SYS_fork, sys_enosys);         REG(SYS_execve, sys_enosys);
    REG(SYS_wait4, sys_enosys);        REG(SYS_kill, sys_enosys);
    REG(SYS_gettimeofday, sys_enosys);

    /* SYSCALL/SYSRET fast path (used once ring-3 processes exist) */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);                       /* EFER.SCE */
    wrmsr(MSR_STAR, ((uint64_t)(GDT_USER_DATA - 8) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x700);                                     /* clear IF, DF, TF */

    int n = 0;
    for (int i = 0; i < SYS_MAX; i++) if (table[i] && table[i] != sys_enosys) n++;
    kprintf("syscall: int 0x80 gate + SYSCALL/LSTAR ready, %d calls implemented\n", n);
}

void syscall_dispatch(struct int_frame *f)
{
    uint64_t nr = f->rax;
    if (nr >= SYS_MAX || !table[nr]) { f->rax = (uint64_t)-ENOSYS; return; }
    counts[nr]++;
    if (f->rflags & (1u << 9)) sti();   /* preemptible if the caller was */
    f->rax = (uint64_t)table[nr](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    cli();
}

const char *syscall_name(int nr) { return (nr >= 0 && nr < SYS_MAX && names[nr]) ? names[nr] : NULL; }
int syscall_implemented(int nr) { return nr >= 0 && nr < SYS_MAX && table[nr] && table[nr] != sys_enosys; }
uint64_t syscall_count(int nr) { return (nr >= 0 && nr < SYS_MAX) ? counts[nr] : 0; }
