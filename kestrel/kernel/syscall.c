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
#include <kernel/storage.h>
#include <kernel/uvm.h>
#include <kernel/socket.h>

extern void syscall_entry(void);

static syscall_fn table[SYS_MAX];
static const char *names[SYS_MAX];
static uint64_t counts[SYS_MAX];

/* Minimal pointer validation. With per-process page tables this becomes
 * copy_from_user()/copy_to_user() with a fault-fixup table. */
static int bad_ptr(uint64_t p)
{
    struct tcb *t = current_task();
    if (p < 4096) return 1;
    return t->user && !uvm_mapped(t->pml4, p, 1);
}

/* [p, p+len) must be mapped user memory for a user process. */
static int bad_buf(uint64_t p, uint64_t len)
{
    struct tcb *t = current_task();
    if (p < 4096) return 1;
    return t->user && len && !uvm_mapped(t->pml4, p, len);
}

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
    if (bad_buf(buf, len)) return -EFAULT;
    return vfs_read(*f, (void *)buf, len);
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (bad_buf(buf, len)) return -EFAULT;
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
    t->fd_cloexec[fd] = (flags & O_CLOEXEC) != 0;
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

/* Permission bits are not enforced on open files; accept and ignore. */
static int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    if (fd >= MAX_FDS || !current_task()->fds[fd]) return -EBADF;
    return 0;
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

/* Single-user system: effective ids are the real ids, a process is its own
 * process group. */
static int64_t sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return pid ? (int64_t)pid : current_task()->pid; }

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

/* Linux clock ids: REALTIME 0, MONOTONIC 1, MONOTONIC_RAW 4, REALTIME_COARSE 5,
 * MONOTONIC_COARSE 6, BOOTTIME 7. The coarse/raw variants are served by the
 * same nanosecond clock (XLibre picks MONOTONIC_COARSE when it exists). */
static int clock_kind(uint64_t clk)
{
    switch (clk) {
    case 0: case 5:         return 0;   /* realtime */
    case 1: case 4: case 6: case 7: return 1;   /* monotonic */
    default:                return -1;
    }
}

static int64_t sys_clock_gettime(uint64_t clk, uint64_t tsp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    int kind = clock_kind(clk);
    if (kind < 0) return -EINVAL;
    if (bad_ptr(tsp)) return -EFAULT;
    struct timespec *ts = (struct timespec *)tsp;
    if (kind == 0) { time_realtime(&ts->tv_sec, &ts->tv_nsec); return 0; }
    uint64_t ns = time_ns();                            /* HPET/TSC, nanosecond resolution */
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static int64_t sys_clock_getres(uint64_t clk, uint64_t tsp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (clock_kind(clk) < 0) return -EINVAL;
    if (!tsp) return 0;
    if (bad_ptr(tsp)) return -EFAULT;
    struct timespec *ts = (struct timespec *)tsp;
    ts->tv_sec = 0;
    ts->tv_nsec = 1;
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
    storage_shutdown();                                 /* NVMe: clean shutdown before the reset */
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

/* ---- user memory (ring-3 processes only) ------------------------------- */
#define PROT_WRITE      0x2
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED_VAL  ((uint64_t)-1)

static uint64_t pg_up(uint64_t a) { return (a + UVM_PAGE - 1) & ~(UVM_PAGE - 1); }

static int64_t sys_brk(uint64_t addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (!t->user) return -ENOSYS;
    if (!addr || addr < t->brk_start || addr >= UVM_MMAP_BASE) return (int64_t)t->brk;
    if (pg_up(addr) > pg_up(t->brk)) {
        if (!uvm_map(t->pml4, pg_up(t->brk), pg_up(addr) - pg_up(t->brk), UVM_W)) return (int64_t)t->brk;
    } else if (pg_up(addr) < pg_up(t->brk)) {
        uvm_unmap(t->pml4, pg_up(addr), pg_up(t->brk) - pg_up(addr));
    }
    t->brk = addr;
    return (int64_t)t->brk;
}

/* Anonymous private memory only for now; file mappings come with the
 * X server's needs (fonts are read(), not mmap()ed, in our build). */
static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off)
{
    (void)prot; (void)off;
    struct tcb *t = current_task();
    if (!t->user) return -ENOSYS;
    if (!len) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS) || (int)fd != -1) return -ENODEV;
    len = pg_up(len);
    uint64_t va;
    if (flags & MAP_FIXED) {
        if (addr & (UVM_PAGE - 1) || !uvm_range_ok(addr, len)) return -EINVAL;
        uvm_unmap(t->pml4, addr, len);                  /* fresh zero pages */
        va = addr;
    } else {
        va = t->mmap_next;
        if (!uvm_range_ok(va, len + UVM_PAGE) || va + len + UVM_PAGE > UVM_STACK_TOP - UVM_STACK_SIZE) return -ENOMEM;
        t->mmap_next = va + len + UVM_PAGE;             /* + one unmapped guard page */
    }
    if (!uvm_map(t->pml4, va, len, UVM_W)) { uvm_unmap(t->pml4, va, len); return -ENOMEM; }
    return (int64_t)va;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (!t->user) return -ENOSYS;
    if (addr & (UVM_PAGE - 1) || !len || !uvm_range_ok(addr, pg_up(len))) return -EINVAL;
    uvm_unmap(t->pml4, addr, pg_up(len));
    return 0;
}

/* Accepted and ignored until W^X is enforced: every user page is RW. */
static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)prot; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (!t->user) return -ENOSYS;
    if (addr & (UVM_PAGE - 1) || !uvm_range_ok(addr, pg_up(len))) return -EINVAL;
    return uvm_mapped(t->pml4, addr, pg_up(len)) ? 0 : -ENOMEM;
}

static int64_t sys_madvise(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return 0; }

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (!t->user) return -ENOSYS;
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= UVM_USER_END) return -EPERM;        /* must stay canonical, user half */
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (bad_buf(addr, 8)) return -EFAULT;
        *(uint64_t *)addr = t->fs_base;
        return 0;
    default:
        return -EINVAL;
    }
}

static int64_t sys_set_tid_address(uint64_t ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    current_task()->tid_address = ptr;
    return current_task()->pid;                         /* one thread per process: tid = pid */
}

static int64_t sys_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->pid; }

/* No signals yet: handlers are accepted and never invoked, masks are empty. */
static int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t old, uint64_t size, uint64_t a5, uint64_t a6)
{
    (void)act; (void)a5; (void)a6;
    if (sig < 1 || sig > 64) return -EINVAL;
    if (old) { if (bad_buf(old, size + 24)) return -EFAULT; memset((void *)old, 0, size + 24); }
    return 0;
}

static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t old, uint64_t size, uint64_t a5, uint64_t a6)
{
    (void)how; (void)set; (void)a5; (void)a6;
    if (old) { if (bad_buf(old, size)) return -EFAULT; memset((void *)old, 0, size); }
    return 0;
}

struct iovec { uint64_t base, len; };

static int64_t rw_vec(uint64_t fd, uint64_t iov, uint64_t cnt, bool wr)
{
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    if (cnt > 1024) return -EINVAL;
    if (cnt && bad_buf(iov, cnt * sizeof(struct iovec))) return -EFAULT;
    const struct iovec *v = (const struct iovec *)iov;
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if (!v[i].len) continue;
        if (bad_buf(v[i].base, v[i].len)) return total ? total : -EFAULT;
        ssize_t n = wr ? vfs_write(*f, (const void *)v[i].base, v[i].len) : vfs_read(*f, (void *)v[i].base, v[i].len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < v[i].len) break;
    }
    return total;
}

static int64_t sys_readv(uint64_t fd, uint64_t iov, uint64_t cnt, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return rw_vec(fd, iov, cnt, false); }

static int64_t sys_writev(uint64_t fd, uint64_t iov, uint64_t cnt, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return rw_vec(fd, iov, cnt, true); }

/* ---- descriptors: fcntl, dup, pipe, poll ------------------------------- */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

static int fd_alloc(struct tcb *t, int min, struct file *f, bool cloexec)
{
    for (int fd = min < 0 ? 0 : min; fd < MAX_FDS; fd++)
        if (!t->fds[fd]) { t->fds[fd] = f; t->fd_cloexec[fd] = cloexec; return fd; }
    return -EMFILE;
}

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    struct file **f = fd_slot((int)fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if ((int)arg < 0 || (int)arg >= MAX_FDS) return -EINVAL;
        int n = fd_alloc(t, (int)arg, *f, cmd == F_DUPFD_CLOEXEC);
        if (n >= 0) (*f)->refcnt++;
        return n;
    }
    case F_GETFD: return t->fd_cloexec[fd] ? FD_CLOEXEC : 0;
    case F_SETFD: t->fd_cloexec[fd] = (arg & FD_CLOEXEC) != 0; return 0;
    case F_GETFL: return (*f)->flags & ~O_CLOEXEC;
    case F_SETFL:                                       /* only these two may change */
        (*f)->flags = ((*f)->flags & ~(O_NONBLOCK | O_APPEND)) | ((int)arg & (O_NONBLOCK | O_APPEND));
        return 0;
    default: return -EINVAL;
    }
}

static int64_t do_dup(int oldfd, int newfd, int flags, bool any)
{
    struct tcb *t = current_task();
    struct file **f = fd_slot(oldfd);
    if (!f) return -EBADF;
    if (any) { int n = fd_alloc(t, 0, *f, false); if (n >= 0) (*f)->refcnt++; return n; }
    if (newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (newfd == oldfd) return flags ? -EINVAL : newfd;   /* dup3 refuses, dup2 is a no-op */
    if (t->fds[newfd]) vfs_close(t->fds[newfd]);
    t->fds[newfd] = *f;
    (*f)->refcnt++;
    t->fd_cloexec[newfd] = (flags & O_CLOEXEC) != 0;
    return newfd;
}

static int64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return do_dup((int)fd, 0, 0, true); }

static int64_t sys_dup2(uint64_t fd, uint64_t nfd, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int)fd == (int)nfd) return fd_slot((int)fd) ? (int64_t)nfd : -EBADF;
    return do_dup((int)fd, (int)nfd, 0, false);
}

static int64_t sys_dup3(uint64_t fd, uint64_t nfd, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)O_CLOEXEC) return -EINVAL;
    return do_dup((int)fd, (int)nfd, (int)flags | 1 /* dup3 */ , false);
}

static int64_t sys_pipe2(uint64_t fds, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (bad_buf(fds, 8)) return -EFAULT;
    if (flags & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    struct file *r, *w;
    int rc = pipe_create(&r, &w, (int)flags);
    if (rc) return rc;
    int a = fd_alloc(t, 0, r, flags & O_CLOEXEC);
    int b = a < 0 ? a : fd_alloc(t, 0, w, flags & O_CLOEXEC);
    if (a < 0 || b < 0) {
        if (a >= 0) t->fds[a] = NULL;
        vfs_close(r); vfs_close(w);
        return -EMFILE;
    }
    ((int *)fds)[0] = a;
    ((int *)fds)[1] = b;
    return 0;
}

static int64_t sys_pipe(uint64_t fds, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; return sys_pipe2(fds, 0, a3, a4, a5, a6); }

struct pollfd { int fd; short events, revents; };

/* Readiness is re-checked every millisecond until something is ready or the
 * timeout expires: simple, and plenty for an X server's event loop on one
 * CPU. Wait queues per object can replace the polling later. */
static int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (nfds > 4 * MAX_FDS) return -EINVAL;
    if (nfds && bad_buf(fds, nfds * sizeof(struct pollfd))) return -EFAULT;
    struct pollfd *p = (struct pollfd *)fds;
    int tmo = (int)timeout;
    uint64_t deadline = tmo > 0 ? time_ms() + (uint64_t)tmo : 0;
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            struct file **f = fd_slot(p[i].fd);
            p[i].revents = f ? (short)vfs_poll(*f, p[i].events | POLLERR | POLLHUP) : POLLNVAL;
            if (p[i].revents) ready++;
        }
        if (ready || tmo == 0) return ready;
        if (tmo > 0 && time_ms() >= deadline) return 0;
        task_sleep_ms(1);
    }
}

/* select(2) and pselect6(2) on top of the same readiness checks as poll:
 * fd_set is a bitmap of 1024 descriptors; only fds below MAX_FDS can be open.
 * The timeout is not written back (Linux does; callers may not rely on it). */
static int64_t do_select(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, int64_t tmo_ms)
{
    if (n > 1024) return -EINVAL;
    uint64_t bytes = (n + 7) / 8;
    uint8_t *sets[3] = { (uint8_t *)rp, (uint8_t *)wp, (uint8_t *)ep };
    uint8_t in[3][MAX_FDS / 8];
    for (int k = 0; k < 3; k++) {
        if (!sets[k] || !bytes) continue;
        if (bad_buf((uint64_t)sets[k], bytes)) return -EFAULT;
        for (uint64_t fd = MAX_FDS; fd < n; fd++)       /* can never be open */
            if (sets[k][fd / 8] & (1u << (fd % 8))) return -EBADF;
        memcpy(in[k], sets[k], bytes < sizeof in[k] ? bytes : sizeof in[k]);
    }
    uint64_t lim = n < MAX_FDS ? n : MAX_FDS;
    static const int want[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI };
    uint64_t deadline = tmo_ms > 0 ? time_ms() + (uint64_t)tmo_ms : 0;
    for (;;) {
        int ready = 0;
        uint8_t out[3][MAX_FDS / 8] = { { 0 } };
        for (uint64_t fd = 0; fd < lim; fd++) {
            for (int k = 0; k < 3; k++) {
                if (!sets[k] || !(in[k][fd / 8] & (1u << (fd % 8)))) continue;
                struct file **f = fd_slot((int)fd);
                if (!f) return -EBADF;
                if (vfs_poll(*f, want[k]) & want[k]) {
                    out[k][fd / 8] |= (uint8_t)(1u << (fd % 8));
                    ready++;
                }
            }
        }
        if (ready || tmo_ms == 0 || (tmo_ms > 0 && time_ms() >= deadline)) {
            for (int k = 0; k < 3; k++) {
                if (!sets[k] || !bytes) continue;
                memset(sets[k], 0, bytes);
                memcpy(sets[k], out[k], bytes < sizeof out[k] ? bytes : sizeof out[k]);
            }
            return ready;
        }
        task_sleep_ms(1);
    }
}

static int64_t sys_select(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, uint64_t tvp, uint64_t a6)
{
    (void)a6;
    int64_t tmo = -1;
    if (tvp) {
        if (bad_buf(tvp, 16)) return -EFAULT;
        const int64_t *tv = (const int64_t *)tvp;     /* struct timeval */
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) return -EINVAL;
        tmo = tv[0] * 1000 + (tv[1] + 999) / 1000;
    }
    return do_select(n, rp, wp, ep, tmo);
}

static int64_t sys_pselect6(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, uint64_t tsp, uint64_t sig)
{
    (void)sig;                                          /* no signals to mask */
    int64_t tmo = -1;
    if (tsp) {
        if (bad_buf(tsp, 16)) return -EFAULT;
        const int64_t *ts = (const int64_t *)tsp;     /* struct timespec */
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -EINVAL;
        tmo = ts[0] * 1000 + (ts[1] + 999999) / 1000000;
    }
    return do_select(n, rp, wp, ep, tmo);
}

static int64_t sys_access(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    if (bad_ptr(path)) return -EFAULT;
    struct stat st;
    return vfs_stat((const char *)path, &st);           /* existence; everyone is root */
}

static int64_t sys_umask(uint64_t mask, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    uint32_t old = t->umask;
    t->umask = (uint32_t)mask & 0777;
    return old;
}

/* RDRAND when the CPU has it, else a TSC-seeded xorshift (fine for X auth
 * cookies on a hobby OS; not for cryptography). */
static int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a4; (void)a5; (void)a6;
    if (len > 1 << 20) len = 1 << 20;
    if (bad_buf(buf, len)) return -EFAULT;
    static int has_rdrand = -1;
    static uint64_t x;
    if (has_rdrand < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        has_rdrand = (c >> 30) & 1;
        x = rdtsc() | 1;
    }
    uint8_t *p = (uint8_t *)buf;
    for (uint64_t i = 0; i < len; i += 8) {
        uint64_t v = 0;
        unsigned char ok = 0;
        if (has_rdrand) __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (!ok) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v = x ^ rdtsc(); }
        for (int k = 0; k < 8 && i + (uint64_t)k < len; k++) p[i + k] = (uint8_t)(v >> (8 * k));
    }
    return (int64_t)len;
}

/* ---- AF_UNIX sockets (fs/unixsock.c) ------------------------------------ */
static int64_t sock_fd(int fd, struct file **out)
{
    struct file **f = fd_slot(fd);
    if (!f) return -EBADF;
    if (!usock_is(*f)) return -ENOTSOCK;
    *out = *f;
    return 0;
}

static int64_t install(struct file *f, bool cloexec)
{
    int fd = fd_alloc(current_task(), 0, f, cloexec);
    if (fd < 0) vfs_close(f);
    return fd;
}

static int64_t sys_socket(uint64_t dom, uint64_t type, uint64_t proto, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f;
    int rc = usock_create((int)dom, (int)type, (int)proto, &f);
    return rc ? rc : install(f, type & SOCK_CLOEXEC);
}

static int64_t sys_socketpair(uint64_t dom, uint64_t type, uint64_t proto, uint64_t sv, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    if (dom != AF_UNIX) return -EAFNOSUPPORT;
    if (proto) return -EPROTONOSUPPORT;
    if (bad_buf(sv, 8)) return -EFAULT;
    struct file *a, *b;
    int rc = usock_pair((int)type, &a, &b);
    if (rc) return rc;
    int64_t x = install(a, type & SOCK_CLOEXEC);
    if (x < 0) { vfs_close(b); return x; }
    int64_t y = install(b, type & SOCK_CLOEXEC);
    if (y < 0) { vfs_close(current_task()->fds[x]); current_task()->fds[x] = NULL; return y; }
    ((int *)sv)[0] = (int)x;
    ((int *)sv)[1] = (int)y;
    return 0;
}

static int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(addr, len)) return -EFAULT;
    return usock_bind(f, (const struct sockaddr_un *)addr, (uint32_t)len);
}

static int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    return rc ? rc : usock_listen(f, (int)backlog);
}

static int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(addr, len)) return -EFAULT;
    return usock_connect(f, (const struct sockaddr_un *)addr, (uint32_t)len);
}

static int64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    uint32_t len = 0;
    if (addr) {
        if (bad_buf(lenp, 4)) return -EFAULT;
        len = *(uint32_t *)lenp;
        if (bad_buf(addr, len)) return -EFAULT;
    }
    struct file *nf;
    rc = usock_accept(f, (int)flags, &nf, addr ? (struct sockaddr_un *)addr : NULL, addr ? &len : NULL);
    if (rc) return rc;
    if (addr) *(uint32_t *)lenp = len;
    return install(nf, flags & SOCK_CLOEXEC);
}

static int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; return sys_accept4(fd, addr, lenp, 0, a5, a6); }

static int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alen)
{
    (void)alen;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (addr) return -EISCONN;                          /* stream sockets only */
    if (bad_buf(buf, len)) return -EFAULT;
    return usock_send(f, (const void *)buf, len, (int)flags);
}

static int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alenp)
{
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(buf, len)) return -EFAULT;
    rc = usock_recv(f, (void *)buf, len, (int)flags);
    if (rc >= 0 && addr && !bad_buf(alenp, 4)) *(uint32_t *)alenp = 0;
    return rc;
}

struct msghdr_k {
    uint64_t name; uint32_t namelen, pad0;
    uint64_t iov, iovlen, control, controllen;
    int32_t  flags, pad1;
};

/* Data only for now: a message carrying ancillary data (SCM_RIGHTS
 * descriptor passing, used by MIT-SHM/DRI3) is refused, never dropped. */
static int64_t msg_io(uint64_t fd, uint64_t msg, uint64_t flags, bool send)
{
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(msg, sizeof(struct msghdr_k))) return -EFAULT;
    struct msghdr_k *m = (struct msghdr_k *)msg;
    if (send && m->controllen) return -EOPNOTSUPP;
    if (m->iovlen > 1024) return -EINVAL;
    if (m->iovlen && bad_buf(m->iov, m->iovlen * sizeof(struct iovec))) return -EFAULT;
    const struct iovec *v = (const struct iovec *)m->iov;
    int64_t total = 0;
    for (uint64_t i = 0; i < m->iovlen; i++) {
        if (!v[i].len) continue;
        if (bad_buf(v[i].base, v[i].len)) return total ? total : -EFAULT;
        long n = send ? usock_send(f, (const void *)v[i].base, v[i].len, (int)flags | (total ? MSG_DONTWAIT : 0))
                      : usock_recv(f, (void *)v[i].base, v[i].len, (int)flags | (total ? MSG_DONTWAIT : 0));
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < v[i].len) break;
    }
    if (!send) { m->controllen = 0; m->flags = 0; if (m->name) m->namelen = 0; }
    return total;
}

static int64_t sys_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return msg_io(fd, msg, flags, true); }

static int64_t sys_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return msg_io(fd, msg, flags, false); }

static int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (how > SHUT_RDWR) return -EINVAL;
    return usock_shutdown(f, (int)how);
}

static int64_t sock_name(uint64_t fd, uint64_t addr, uint64_t lenp, bool peer)
{
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(lenp, 4)) return -EFAULT;
    uint32_t len = *(uint32_t *)lenp;
    if (len && bad_buf(addr, len)) return -EFAULT;
    struct sockaddr_un tmp;
    uint32_t got = sizeof tmp;
    if ((rc = usock_name(f, peer, &tmp, &got))) return rc;
    memcpy((void *)addr, &tmp, got < len ? got : len);
    *(uint32_t *)lenp = got;
    return 0;
}

static int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return sock_name(fd, addr, lenp, false); }

static int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return sock_name(fd, addr, lenp, true); }

/* Options are accepted and ignored (SO_REUSEADDR, buffer sizes, ...). */
static int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t len, uint64_t a6)
{
    (void)level; (void)opt; (void)val; (void)len; (void)a6;
    struct file *f;
    return sock_fd((int)fd, &f);
}

static int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t lenp, uint64_t a6)
{
    (void)a6;
    struct file *f; int64_t rc = sock_fd((int)fd, &f);
    if (rc) return rc;
    if (bad_buf(lenp, 4)) return -EFAULT;
    uint32_t len = *(uint32_t *)lenp;
    if (bad_buf(val, len)) return -EFAULT;
    rc = usock_getsockopt(f, (int)level, (int)opt, (void *)val, &len);
    if (!rc) *(uint32_t *)lenp = len;
    return rc;
}

/* Ring-3 processes cannot create processes yet (no fork/clone), so a
 * process never has children to wait for. */
static int64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options, uint64_t ru, uint64_t a5, uint64_t a6)
{
    (void)pid; (void)status; (void)options; (void)ru; (void)a5; (void)a6;
    return -ECHILD;
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
    REG(SYS_clock_getres, sys_clock_getres);
    REG(SYS_lstat, sys_stat);          /* no symlinks: lstat == stat */
    REG(SYS_geteuid, sys_getuid);      REG(SYS_getegid, sys_getgid);
    REG(SYS_getpgid, sys_getpgid);     REG(SYS_fchmod, sys_fchmod);
    REG(SYS_reboot, sys_reboot);
    /* placeholders: need user address spaces / signals */
    REG(SYS_mmap, sys_mmap);           REG(SYS_brk, sys_brk);
    REG(SYS_munmap, sys_munmap);       REG(SYS_mprotect, sys_mprotect);
    REG(SYS_madvise, sys_madvise);     REG(SYS_arch_prctl, sys_arch_prctl);
    REG(SYS_set_tid_address, sys_set_tid_address);
    REG(SYS_gettid, sys_gettid);
    REG(SYS_rt_sigaction, sys_rt_sigaction);
    REG(SYS_rt_sigprocmask, sys_rt_sigprocmask);
    REG(SYS_readv, sys_readv);         REG(SYS_writev, sys_writev);
    REG(SYS_fcntl, sys_fcntl);         REG(SYS_poll, sys_poll);
    REG(SYS_select, sys_select);       REG(SYS_pselect6, sys_pselect6);
    REG(SYS_dup, sys_dup);             REG(SYS_dup2, sys_dup2);
    REG(SYS_dup3, sys_dup3);           REG(SYS_pipe, sys_pipe);
    REG(SYS_pipe2, sys_pipe2);         REG(SYS_access, sys_access);
    REG(SYS_umask, sys_umask);         REG(SYS_getrandom, sys_getrandom);
    REG(SYS_socket, sys_socket);       REG(SYS_socketpair, sys_socketpair);
    REG(SYS_bind, sys_bind);           REG(SYS_listen, sys_listen);
    REG(SYS_connect, sys_connect);     REG(SYS_accept, sys_accept);
    REG(SYS_accept4, sys_accept4);     REG(SYS_sendto, sys_sendto);
    REG(SYS_recvfrom, sys_recvfrom);   REG(SYS_sendmsg, sys_sendmsg);
    REG(SYS_recvmsg, sys_recvmsg);     REG(SYS_shutdown, sys_shutdown);
    REG(SYS_getsockname, sys_getsockname); REG(SYS_getpeername, sys_getpeername);
    REG(SYS_setsockopt, sys_setsockopt);   REG(SYS_getsockopt, sys_getsockopt);
    REG(SYS_fork, sys_enosys);         REG(SYS_execve, sys_enosys);
    REG(SYS_wait4, sys_wait4);        REG(SYS_kill, sys_enosys);
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
    if (nr >= SYS_MAX || !table[nr] || table[nr] == sys_enosys) {
        static uint8_t warned[SYS_MAX + 1];
        struct tcb *t = current_task();
        uint64_t k = nr < SYS_MAX ? nr : SYS_MAX;
        if (t->user && warned[k] < 3) {         /* the gaps a ported program hits */
            warned[k]++;
            kprintf("syscall: pid %d (%s) called unimplemented syscall %lu (%s) at %lx\n", t->pid, t->name, nr,
                    nr < SYS_MAX && names[nr] ? names[nr] : "?", f->rip);
        }
        f->rax = (uint64_t)-ENOSYS;
        return;
    }
    counts[nr]++;
    if (f->rflags & (1u << 9)) sti();   /* preemptible if the caller was */
    f->rax = (uint64_t)table[nr](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    cli();
}

const char *syscall_name(int nr) { return (nr >= 0 && nr < SYS_MAX && names[nr]) ? names[nr] : NULL; }
int syscall_implemented(int nr) { return nr >= 0 && nr < SYS_MAX && table[nr] && table[nr] != sys_enosys; }
uint64_t syscall_count(int nr) { return (nr >= 0 && nr < SYS_MAX) ? counts[nr] : 0; }
