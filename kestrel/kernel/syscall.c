/* kernel/syscall.c -- system-call table and handlers
 *
 * Linux x86_64 numbers and semantics. Arguments from user space are treated
 * as hostile: every pointer goes through uaccess.h (range check + fault-
 * tolerant copy, see there), paths are copied into kernel buffers with a
 * length limit before anything looks at them, data moves through kernel
 * bounce buffers, and structures are copied whole -- the kernel never reads
 * a user field twice (no time-of-check/time-of-use window). Open files are
 * looked up with a reference held (fget/fput), so a close() from another
 * thread cannot free one under a running call. */
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/mm.h>
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
#include <kernel/uaccess.h>
#include <kernel/termios.h>
#include <kernel/kfb.h>
#include <kernel/kinput.h>
#include <kernel/vm.h>
#include <kernel/vmobj.h>
#include <kernel/futex.h>

extern void syscall_entry(void);

static syscall_fn table[SYS_MAX];
static const char *names[SYS_MAX];
static uint64_t counts[SYS_MAX];

#define UNUSED6(a, b, c, d, e, f) do { (void)(a); (void)(b); (void)(c); (void)(d); (void)(e); (void)(f); } while (0)
#define IO_CHUNK    65536u                  /* bounce buffer for read/write and friends */

/* A user buffer the call may touch: inside the user half (kernel threads
 * calling through int 0x80 pass kernel buffers). */
static bool ubuf_ok(uint64_t p, uint64_t len)
{
    struct tcb *t = current_task();
    return !t->user || user_range_ok(p, len);
}

/* ---- file descriptors ---------------------------------------------------- */
/* The open file behind fd, with a reference the caller drops with fput(). */
static struct file *fget(int fd)
{
    struct tcb *t = current_task();
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    uint64_t fl = irq_save();
    struct file *f = vfs_file_get(t->files->fd[fd]);
    irq_restore(fl);
    return f;
}
static void fput(struct file *f) { vfs_close(f); }

/* Install f (whose reference the table takes over) at the lowest free
 * descriptor >= min. -EMFILE if there is none (f is not consumed then). */
static int fd_alloc(struct tcb *t, int min, struct file *f, bool cloexec)
{
    uint64_t fl = irq_save();
    for (int fd = min < 0 ? 0 : min; fd < MAX_FDS; fd++)
        if (!t->files->fd[fd]) {
            t->files->fd[fd] = f;
            t->files->cloexec[fd] = cloexec;
            irq_restore(fl);
            return fd;
        }
    irq_restore(fl);
    return -EMFILE;
}

/* Remove fd from the table and return its file (the table's reference). */
static struct file *fd_take(struct tcb *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    uint64_t fl = irq_save();
    struct file *f = t->files->fd[fd];
    t->files->fd[fd] = NULL;
    t->files->cloexec[fd] = 0;
    irq_restore(fl);
    return f;
}

/* Install a new file; on failure the file is closed. */
static int64_t install(struct file *f, bool cloexec)
{
    int fd = fd_alloc(current_task(), 0, f, cloexec);
    if (fd < 0) vfs_close(f);
    return fd;
}

/* ---- paths ------------------------------------------------------------------ */
/* Copy a user path into buf[VFS_PATH_MAX]. */
static int get_path(uint64_t up, char *buf)
{
    long n = strncpy_from_user(buf, (const char *)up, VFS_PATH_MAX);
    if (n < 0) return (int)n;
    return n ? 0 : -ENOENT;                             /* "" names nothing */
}

/* ---- file I/O ------------------------------------------------------------- */
/* Read through a bounce buffer. Regular files are read until the request is
 * satisfied or EOF; anything else (pipes, sockets, terminals, devices) gets
 * exactly one read, so the call returns as soon as some data is there. */
static int64_t read_to_user(struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!len) return 0;
    if (!ubuf_ok(ubuf, len)) return -EFAULT;
    size_t chunk = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
    uint8_t *kb = kmalloc(chunk);
    if (!kb) return -ENOMEM;
    bool regular = f->vn && f->vn->type == VREG;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? (size_t)(len - (uint64_t)total) : chunk;
        ssize_t n = vfs_read(f, kb, want);
        if (n < 0) { if (!total) total = n; break; }
        if (n == 0) break;
        if (copy_to_user((void *)(ubuf + (uint64_t)total), kb, (size_t)n)) { if (!total) total = -EFAULT; break; }
        total += n;
        if ((size_t)n < want || !regular) break;
    }
    kfree(kb);
    return total;
}

/* Write through a bounce buffer, chunk by chunk, until all is written or a
 * write comes up short (non-blocking pipe full, disk full, ...). */
static int64_t write_from_user(struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!len) return vfs_write(f, "", 0);
    if (!ubuf_ok(ubuf, len)) return -EFAULT;
    size_t chunk = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
    uint8_t *kb = kmalloc(chunk);
    if (!kb) return -ENOMEM;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? (size_t)(len - (uint64_t)total) : chunk;
        if (copy_from_user(kb, (const void *)(ubuf + (uint64_t)total), want)) { if (!total) total = -EFAULT; break; }
        ssize_t n = vfs_write(f, kb, want);
        if (n < 0) { if (!total) total = n; break; }
        total += n;
        if ((size_t)n < want) break;
    }
    kfree(kb);
    return total;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if ((int64_t)len < 0) return -EINVAL;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t r = read_to_user(f, buf, len);
    fput(f);
    return r;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if ((int64_t)len < 0) return -EINVAL;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t r = write_from_user(f, buf, len);
    fput(f);
    return r;
}

/* pread64/pwrite64: at an offset, without moving the file position. */
static int64_t pio(uint64_t fd, uint64_t buf, uint64_t len, int64_t off, bool wr)
{
    if ((int64_t)len < 0 || off < 0) return -EINVAL;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t total = 0;
    uint8_t *kb = NULL;
    if (!len) goto out;
    if (!ubuf_ok(buf, len)) { total = -EFAULT; goto out; }
    size_t chunk = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
    if (!(kb = kmalloc(chunk))) { total = -ENOMEM; goto out; }
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? (size_t)(len - (uint64_t)total) : chunk;
        ssize_t n;
        if (wr) {
            if (copy_from_user(kb, (const void *)(buf + (uint64_t)total), want)) { if (!total) total = -EFAULT; break; }
            n = vfs_pwrite(f, kb, want, (uint64_t)off + (uint64_t)total);
        } else {
            n = vfs_pread(f, kb, want, (uint64_t)off + (uint64_t)total);
            if (n > 0 && copy_to_user((void *)(buf + (uint64_t)total), kb, (size_t)n)) { if (!total) total = -EFAULT; break; }
        }
        if (n < 0) { if (!total) total = n; break; }
        total += n;
        if ((size_t)n < want) break;
    }
out:
    kfree(kb);
    fput(f);
    return total;
}
static int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off, uint64_t a5, uint64_t a6)
{ (void)a5; (void)a6; return pio(fd, buf, len, (int64_t)off, false); }
static int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off, uint64_t a5, uint64_t a6)
{ (void)a5; (void)a6; return pio(fd, buf, len, (int64_t)off, true); }

static int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    struct file *f;
    r = vfs_open(p, (int)flags, (mode_t)mode, &f);
    if (r < 0) return r;
    return install(f, (flags & O_CLOEXEC) != 0);
}

#define AT_FDCWD   -100
/* openat and the other *at calls: AT_FDCWD or absolute paths only. */
static int64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    struct file *f;
    r = vfs_open(p, (int)flags, (mode_t)mode, &f);
    if (r < 0) return r;
    return install(f, (flags & O_CLOEXEC) != 0);
}

static int64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f = fd_take(current_task(), (int)fd);
    if (!f) return -EBADF;
    vfs_close(f);
    return 0;
}

static int64_t sys_stat(uint64_t path, uint64_t st, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    struct stat ks;
    memset(&ks, 0, sizeof ks);
    if ((r = vfs_stat(p, &ks)) < 0) return r;
    return copy_to_user((void *)st, &ks, sizeof ks) ? -EFAULT : 0;
}

static int64_t sys_fstat(uint64_t fd, uint64_t st, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    struct stat ks;
    memset(&ks, 0, sizeof ks);
    int r = vfs_fstat(f, &ks);
    fput(f);
    if (r < 0) return r;
    return copy_to_user((void *)st, &ks, sizeof ks) ? -EFAULT : 0;
}

/* newfstatat(dirfd, path, st, flags): AT_FDCWD, absolute paths, or
 * AT_EMPTY_PATH on a descriptor. */
#define AT_EMPTY_PATH 0x1000
static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t st, uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    long n = strncpy_from_user(p, (const char *)path, sizeof p);
    if (n < 0) return n;
    if (n == 0) {
        if (!(flags & AT_EMPTY_PATH)) return -ENOENT;
        return sys_fstat(dirfd, st, 0, 0, 0, 0);
    }
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    return sys_stat(path, st, 0, 0, 0, 0);
}

static int64_t sys_lseek(uint64_t fd, uint64_t off, uint64_t whence, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t r = vfs_lseek(f, (off_t)off, (int)whence);
    fput(f);
    return r;
}

/* ioctl: the argument is a pointer for most requests, and the size and
 * direction of what it points to are not part of the old tty request
 * numbers -- so they come from this table. The structure is copied into a
 * kernel buffer, the driver works on that, and results are copied back:
 * drivers never see a user pointer. Requests whose argument is a plain value
 * (and unknown ones) pass the value through. */
#define IOC_IN  1
#define IOC_OUT 2
static const struct { uint32_t req; uint16_t size; uint8_t dir; } ioctl_args[] = {
    { TCGETS, sizeof(struct ktermios), IOC_OUT },  { TCSETS, sizeof(struct ktermios), IOC_IN },
    { TCSETSW, sizeof(struct ktermios), IOC_IN },  { TCSETSF, sizeof(struct ktermios), IOC_IN },
    { TIOCGWINSZ, sizeof(struct kwinsize), IOC_OUT }, { TIOCSWINSZ, sizeof(struct kwinsize), IOC_IN },
    { TIOCGPGRP, 4, IOC_OUT }, { TIOCSPGRP, 4, IOC_IN }, { TIOCOUTQ, 4, IOC_OUT }, { FIONREAD_T, 4, IOC_OUT },
    { FIONBIO, 4, IOC_IN }, { TIOCGETD, 4, IOC_OUT }, { TIOCSETD, 4, IOC_IN }, { TIOCGSID, 4, IOC_OUT },
    { TIOCGPTN, 4, IOC_OUT }, { TIOCSPTLCK, 4, IOC_IN },
    { KFB_GET_INFO, sizeof(struct kfb_info), IOC_OUT }, { KFB_BLIT, sizeof(struct kfb_blit), IOC_IN },
    { FBIOGET_VSCREENINFO, sizeof(struct fb_var_screeninfo_lite), IOC_OUT },
    { KINPUT_GRAB, 4, IOC_IN },
};

static int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int size = -1, dir = 0;
    for (size_t i = 0; i < sizeof ioctl_args / sizeof *ioctl_args; i++)
        if (ioctl_args[i].req == (uint32_t)req) { size = ioctl_args[i].size; dir = ioctl_args[i].dir; break; }
    int64_t r;
    if (size < 0) {
        r = vfs_ioctl(f, (unsigned long)req, (void *)arg);       /* a value, or unknown */
    } else {
        uint64_t kb[64];                                        /* 512 bytes, 8-aligned */
        memset(kb, 0, sizeof kb);
        if ((dir & IOC_IN) && copy_from_user(kb, (const void *)arg, (size_t)size)) { r = -EFAULT; goto out; }
        if ((dir & IOC_OUT) && !ubuf_ok(arg, (uint64_t)size)) { r = -EFAULT; goto out; }
        r = vfs_ioctl(f, (unsigned long)req, kb);
        if (r >= 0 && (dir & IOC_OUT) && copy_to_user((void *)arg, kb, (size_t)size)) r = -EFAULT;
    }
out:
    fput(f);
    return r;
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    size_t n = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
    int64_t r;
    uint8_t *kb = n ? kmalloc(n) : NULL;
    if (n && !kb) { r = -ENOMEM; goto out; }
    if (!ubuf_ok(buf, n)) { r = -EFAULT; goto out; }
    r = vfs_getdents(f, kb, n);
    if (r > 0 && copy_to_user((void *)buf, kb, (size_t)r)) r = -EFAULT;
out:
    kfree(kb);
    fput(f);
    return r;
}

/* ---- paths -------------------------------------------------------------- */
static int64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    char k[VFS_PATH_MAX];
    int r = vfs_getcwd(k, sizeof k);
    if (r < 0) return r;
    size_t n = strlen(k) + 1;
    if (n > size) return -ERANGE;
    return copy_to_user((void *)buf, k, n) ? -EFAULT : (int64_t)n;
}

#define PATH_CALL(name, expr)                                                       \
static int64_t name(uint64_t path, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) \
{                                                                                    \
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;                               \
    char p[VFS_PATH_MAX];                                                            \
    int r = get_path(path, p);                                                       \
    return r ? r : (expr);                                                           \
}
PATH_CALL(sys_chdir, vfs_chdir(p))
PATH_CALL(sys_mkdir, vfs_mkdir(p, (mode_t)a2))
PATH_CALL(sys_unlink, vfs_unlink(p))
PATH_CALL(sys_rmdir, vfs_rmdir(p))
PATH_CALL(sys_chmod, vfs_chmod(p, (mode_t)a2))

static int64_t sys_rename(uint64_t from, uint64_t to, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    int r = get_path(from, a);
    if (!r) r = get_path(to, b);
    return r ? r : vfs_rename(a, b);
}

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    return (flags & 0x200) ? vfs_rmdir(p) : vfs_unlink(p);     /* AT_REMOVEDIR */
}

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    return vfs_mkdir(p, (mode_t)mode);
}

/* Permission bits are not enforced on open files; accept and ignore. */
static int64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    fput(f);
    return 0;
}

static int64_t sys_chown(uint64_t path, uint64_t uid, uint64_t gid, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if (current_task()->uid != 0) return -EPERM;        /* only root may give files away */
    return vfs_chown(p, (int)uid, (int)gid);
}

/* utimensat(dirfd, path, times[2], flags): AT_FDCWD (or absolute paths)
 * only; times NULL or UTIME_NOW = now. Only the modification time is kept. */
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
static int64_t sys_utimensat(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    int64_t mtime = -1;
    if (times) {
        struct timespec ts[2];
        if (copy_from_user(ts, (const void *)times, sizeof ts)) return -EFAULT;
        if (ts[1].tv_nsec == UTIME_OMIT) return 0;
        if (ts[1].tv_nsec != UTIME_NOW) {
            if (ts[1].tv_nsec < 0 || ts[1].tv_nsec >= 1000000000) return -EINVAL;
            mtime = ts[1].tv_sec;
        }
    }
    return vfs_utime(p, mtime);
}

/* ---- processes ---------------------------------------------------------- */
static int64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); return current_task()->tgid; }      /* the process; gettid: the thread */

static int64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); return current_task()->ppid; }

static int64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); return current_task()->uid; }

static int64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); return current_task()->gid; }

static int64_t sys_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); sched_yield(); return 0; }

static int64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; task_exit((int)(code & 0xff)); }      /* this thread */

static int64_t sys_exit_group(uint64_t code, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; proc_exit_group((int)(code & 0xff)); }

static int timespec_ok(const struct timespec *ts) { return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000; }

static int64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct timespec ts;
    if (copy_from_user(&ts, (const void *)req, sizeof ts)) return -EFAULT;
    if (!timespec_ok(&ts)) return -EINVAL;
    uint64_t ms = (uint64_t)ts.tv_sec > (1ull << 40) ? (1ull << 50) : (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    uint64_t deadline = time_ms() + ms;
    for (uint64_t now; (now = time_ms()) < deadline; ) {
        if (signal_pending()) {                         /* EINTR with the time left */
            if (rem) {
                struct timespec r = { (int64_t)((deadline - now) / 1000), (int64_t)((deadline - now) % 1000) * 1000000 };
                if (copy_to_user((void *)rem, &r, sizeof r)) return -EFAULT;
            }
            return -EINTR;
        }
        task_sleep_ms(deadline - now);                  /* a signal ends it early */
    }
    return 0;
}

/* Linux clock ids: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME 2, THREAD_CPUTIME 3,
 * MONOTONIC_RAW 4, REALTIME_COARSE 5, MONOTONIC_COARSE 6, BOOTTIME 7. */
static int clock_kind(uint64_t clk)
{
    switch (clk) {
    case 0: case 5:                 return 0;   /* realtime */
    case 1: case 4: case 6: case 7: return 1;   /* monotonic */
    case 2: case 3:                 return 2;   /* CPU time of the process/thread */
    default:                        return -1;
    }
}

static void clock_read(int kind, struct timespec *ts)
{
    if (kind == 0) { time_realtime(&ts->tv_sec, &ts->tv_nsec); return; }
    uint64_t ns = kind == 1 ? time_ns() : current_task()->cpu_ticks * (1000000000ull / PIT_HZ);
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
}

static int64_t sys_clock_gettime(uint64_t clk, uint64_t tsp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    int kind = clock_kind(clk);
    if (kind < 0) return -EINVAL;
    struct timespec ts;
    clock_read(kind, &ts);
    return copy_to_user((void *)tsp, &ts, sizeof ts) ? -EFAULT : 0;
}

static int64_t sys_clock_getres(uint64_t clk, uint64_t tsp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (clock_kind(clk) < 0) return -EINVAL;
    if (!tsp) return 0;
    struct timespec ts = { 0, 1 };
    return copy_to_user((void *)tsp, &ts, sizeof ts) ? -EFAULT : 0;
}

static int64_t sys_gettimeofday(uint64_t tvp, uint64_t tzp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (tvp) {
        struct timespec ts;
        clock_read(0, &ts);
        int64_t tv[2] = { ts.tv_sec, ts.tv_nsec / 1000 };
        if (copy_to_user((void *)tvp, tv, sizeof tv)) return -EFAULT;
    }
    if (tzp) { int32_t tz[2] = { 0, 0 }; if (copy_to_user((void *)tzp, tz, sizeof tz)) return -EFAULT; }
    return 0;
}

static int64_t sys_uname(uint64_t p, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct utsname u;
    memset(&u, 0, sizeof u);
    strlcpy(u.sysname, KESTREL_NAME, sizeof u.sysname);
    strlcpy(u.nodename, KESTREL_HOSTNAME, sizeof u.nodename);
    strlcpy(u.release, KESTREL_RELEASE, sizeof u.release);
    strlcpy(u.version, KESTREL_CODENAME, sizeof u.version);
    strlcpy(u.machine, KESTREL_MACHINE, sizeof u.machine);
    strlcpy(u.domainname, "(none)", sizeof u.domainname);
    return copy_to_user((void *)p, &u, sizeof u) ? -EFAULT : 0;
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

/* ---- user memory (mm/vm.c does the work) ------------------------------- */
static struct mm *cur_mm(void) { struct tcb *t = current_task(); return t->user ? t->mm : NULL; }

static int64_t sys_brk(uint64_t addr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct mm *mm = cur_mm();
    if (!mm) return -ENOSYS;
    return vm_brk(mm, addr);
}

/* mmap(2): anonymous (private or shared), private file mappings (filled from
 * the file on first touch, beyond EOF: SIGBUS), shared mappings of files
 * that keep their data in a vm_object (ramfs, memfd: coherent with
 * read/write), /dev/zero. A shared read-only mapping of another regular file
 * (the boot volume) is the same as a private one. */
#define MAP_KNOWN (MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_DENYWRITE | MAP_EXECUTABLE | \
                   MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE | MAP_NONBLOCK | MAP_STACK | MAP_FIXED_NOREPLACE)

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off)
{
    struct mm *mm = cur_mm();
    if (!mm) return -ENOSYS;
    int type = (int)(flags & MAP_TYPE);
    if (type != MAP_SHARED && type != MAP_PRIVATE && type != MAP_SHARED_VALIDATE) return -EINVAL;
    if (type == MAP_SHARED_VALIDATE && (flags & ~(uint64_t)MAP_KNOWN)) return -EOPNOTSUPP;
    if (flags & MAP_HUGETLB) return -EINVAL;
    if (!len || (off & (UVM_PAGE - 1))) return -EINVAL;
    if (len > UVM_USER_END - UVM_USER_START) return -ENOMEM;
    bool shared = type != MAP_PRIVATE;
    if (flags & MAP_ANONYMOUS) {
        if (!shared) return vm_mmap(mm, addr, len, (int)prot, (int)flags, NULL, NULL, 0, 0);
        struct vm_object *o = vmobj_new((len + UVM_PAGE - 1) & ~(UVM_PAGE - 1));
        if (!o) return -ENOMEM;
        int64_t r = vm_mmap(mm, addr, len, (int)prot, (int)flags, NULL, o, 0, 0);
        vmobj_put(o);                                   /* the mapping holds it now */
        return r;
    }
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t r;
    int acc = f->flags & O_ACCMODE;
    struct vnode *vn = f->vn;
    if (acc == O_WRONLY) { r = -EACCES; goto out; }
    if (vn->type == VCHR && vn->rdev == ((1u << 8) | 5)) {          /* /dev/zero */
        r = sys_mmap(addr, len, prot, flags | MAP_ANONYMOUS, (uint64_t)-1, 0);
        goto out;
    }
    if (vn->type != VREG) { r = -ENODEV; goto out; }
    struct vm_object *o = shared ? vfs_vmobject(vn) : NULL;
    if (shared && o) {
        if ((prot & PROT_WRITE) && acc != O_RDWR) { r = -EACCES; goto out; }
        if ((prot & PROT_WRITE) && (o->seals & 0x0008)) { r = -EPERM; goto out; }   /* F_SEAL_WRITE */
        r = vm_mmap(mm, addr, len, (int)prot, (int)flags, NULL, o, off, 0);
    } else {
        if (shared && (prot & PROT_WRITE)) { r = -EACCES; goto out; }  /* read-only file system */
        r = vm_mmap(mm, addr, len, (int)prot, (int)flags, f, NULL, off, UINT64_MAX);
    }
out:
    fput(f);
    return r;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct mm *mm = cur_mm();
    return mm ? vm_munmap(mm, addr, len) : -ENOSYS;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct mm *mm = cur_mm();
    return mm ? vm_mprotect(mm, addr, len, (int)prot) : -ENOSYS;
}

static int64_t sys_mremap(uint64_t old, uint64_t olen, uint64_t nlen, uint64_t flags, uint64_t naddr, uint64_t a6)
{
    (void)a6;
    struct mm *mm = cur_mm();
    return mm ? vm_mremap(mm, old, olen, nlen, (int)flags, naddr) : -ENOSYS;
}

#define MADV_DONTNEED 4
#define MADV_FREE     8
static int64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct mm *mm = cur_mm();
    if (!mm) return -ENOSYS;
    if (addr & (UVM_PAGE - 1)) return -EINVAL;
    if (advice == MADV_DONTNEED || advice == MADV_FREE) return vm_madvise_dontneed(mm, addr, len);
    return 0;                                           /* hints: accepted */
}

static int64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)len; (void)a4; (void)a5; (void)a6;
    if ((addr & (UVM_PAGE - 1)) || (flags & ~7ull) || ((flags & 1) && (flags & 4))) return -EINVAL;
    return 0;                                           /* shared mappings are the file's own pages */
}

static int64_t sys_mincore(uint64_t addr, uint64_t len, uint64_t vec, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct mm *mm = cur_mm();
    if (!mm) return -ENOSYS;
    if (addr & (UVM_PAGE - 1)) return -EINVAL;
    uint64_t pages = (len + UVM_PAGE - 1) / UVM_PAGE;
    if (!user_range_ok(addr, pages * UVM_PAGE)) return -ENOMEM;
    if (pages > (1u << 20)) return -ENOMEM;
    uint8_t *k = pages ? kmalloc(pages) : NULL;
    if (pages && !k) return -ENOMEM;
    int r = vm_mincore(mm, addr, pages * UVM_PAGE, k);
    if (!r && copy_to_user((void *)vec, k, pages)) r = -EFAULT;
    kfree(k);
    return r;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int64_t)len < 0) return -EINVAL;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int r = (f->flags & O_ACCMODE) == O_RDONLY ? -EINVAL : vfs_truncate(f->vn, len);
    fput(f);
    return r;
}

static int64_t sys_truncate(uint64_t path, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int64_t)len < 0) return -EINVAL;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    struct vnode *vn;
    if ((r = vfs_lookup(p, &vn))) return r;
    return vfs_truncate(vn, len);
}

static int64_t sys_fsync(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    fput(f);
    return 0;                                           /* everything lives in memory */
}

/* memfd_create(name, flags): an anonymous ramfs file (page-backed, so it can
 * be shared with MAP_SHARED and passed to other processes). */
#define MFD_CLOEXEC       1
#define MFD_ALLOW_SEALING 2
static int64_t sys_memfd_create(uint64_t uname, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING | 4 /* MFD_HUGETLB */)) return -EINVAL;
    if (flags & 4) return -EINVAL;
    char name[250 + 7];
    strlcpy(name, "memfd:", sizeof name);
    long n = strncpy_from_user(name + 6, (const char *)uname, sizeof name - 6);
    if (n == -EFAULT) return -EFAULT;
    if (n < 0) return -EINVAL;                          /* longer than 249 bytes */
    struct file *f = ramfs_anon_file(name, O_RDWR);
    if (!f) return -ENOMEM;
    struct vm_object *o = vfs_vmobject(f->vn);
    if (!o) { vfs_close(f); return -ENOMEM; }
    if (!(flags & MFD_ALLOW_SEALING)) o->seals = 0x0001;   /* F_SEAL_SEAL: no seals can be added */
    return install(f, flags & MFD_CLOEXEC);
}

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
        return put_user_u64((void *)addr, t->fs_base);
    default:
        return -EINVAL;
    }
}

static int64_t sys_set_tid_address(uint64_t ptr, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    current_task()->tid_address = ptr;                  /* checked when it is written */
    return current_task()->pid;
}

static int64_t sys_gettid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ UNUSED6(a1, a2, a3, a4, a5, a6); return current_task()->pid; }

/* ---- threads ------------------------------------------------------------- */
static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t utime, uint64_t uaddr2, uint64_t val3)
{ return sys_futex_call(uaddr, (int)op, (uint32_t)val, utime, uaddr2, (uint32_t)val3); }

/* The head of the robust-futex list (struct robust_list_head: next,
 * futex_offset, list_op_pending). It is read only when the thread exits. */
static int64_t sys_set_robust_list(uint64_t head, uint64_t len, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (len != 24) return -EINVAL;
    current_task()->robust_list = head;
    return 0;
}

static int64_t sys_get_robust_list(uint64_t tid, uint64_t uhead, uint64_t ulen, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct tcb *self = current_task(), *t = tid ? task_find((int)tid) : self;
    if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || !t->user) return -ESRCH;
    if (t->signal != self->signal && t->uid != self->uid && self->uid) return -EPERM;
    if (put_user_u64((void *)uhead, t->robust_list) || put_user_u64((void *)ulen, 24)) return -EFAULT;
    return 0;
}

/* One CPU (for now): the mask always has bit 0. */
static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t len, uint64_t umask, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (pid && !task_find((int)pid)) return -ESRCH;
    if (len < 8 || (len & 7)) return -EINVAL;
    if (len > 128) len = 128;
    uint8_t buf[128] = { 1 };
    if (copy_to_user((void *)umask, buf, len)) return -EFAULT;
    return (int64_t)len;
}

static int64_t sys_sched_setaffinity(uint64_t pid, uint64_t len, uint64_t umask, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (pid && !task_find((int)pid)) return -ESRCH;
    uint8_t first;
    if (!len) return -EINVAL;
    if (copy_from_user(&first, (const void *)umask, 1)) return -EFAULT;
    return (first & 1) ? 0 : -EINVAL;                   /* CPU 0 must be in it */
}

struct iovec { uint64_t base, len; };

/* readv/writev: the vector is copied in whole, then each segment goes
 * through the same bounce-buffered path as read/write. */
static int64_t rw_vec(uint64_t fd, uint64_t iov, uint64_t cnt, bool wr)
{
    if (cnt > 1024) return -EINVAL;
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t total = 0;
    struct iovec *v = cnt ? kmalloc(cnt * sizeof *v) : NULL;
    if (cnt && !v) { total = -ENOMEM; goto out; }
    if (copy_from_user(v, (const void *)iov, cnt * sizeof *v)) { total = -EFAULT; goto out; }
    uint64_t sum = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if ((int64_t)v[i].len < 0 || sum + v[i].len < sum) { total = -EINVAL; goto out; }
        sum += v[i].len;
    }
    for (uint64_t i = 0; i < cnt; i++) {
        if (!v[i].len) continue;
        int64_t n = wr ? write_from_user(f, v[i].base, v[i].len) : read_to_user(f, v[i].base, v[i].len);
        if (n < 0) { if (!total) total = n; break; }
        total += n;
        if ((uint64_t)n < v[i].len) break;
    }
out:
    kfree(v);
    fput(f);
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

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    struct file *f = fget((int)fd);
    if (!f) return -EBADF;
    int64_t r;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if ((int)arg < 0 || (int)arg >= MAX_FDS) { r = -EINVAL; break; }
        r = fd_alloc(t, (int)arg, vfs_file_get(f), cmd == F_DUPFD_CLOEXEC);
        if (r < 0) vfs_close(f);                        /* the reference meant for the table */
        break;
    case F_GETFD: r = t->files->cloexec[fd] ? FD_CLOEXEC : 0; break;
    case F_SETFD: t->files->cloexec[fd] = (arg & FD_CLOEXEC) != 0; r = 0; break;
    case F_GETFL: r = f->flags & ~O_CLOEXEC; break;
    case 1033: {                                        /* F_ADD_SEALS */
        struct vm_object *o = f->vn->type == VREG ? vfs_vmobject(f->vn) : NULL;
        if (!o || !f->vn->unlinked) { r = -EINVAL; break; }     /* memfds only */
        if (o->seals & 0x0001) { r = -EPERM; break; }           /* F_SEAL_SEAL */
        if (arg & ~0x1fULL) { r = -EINVAL; break; }
        if ((f->flags & O_ACCMODE) == O_RDONLY) { r = -EPERM; break; }
        o->seals |= (uint32_t)arg;
        r = 0;
        break;
    }
    case 1034: {                                        /* F_GET_SEALS */
        struct vm_object *o = f->vn->type == VREG ? vfs_vmobject(f->vn) : NULL;
        r = o && f->vn->unlinked ? (int64_t)o->seals : -EINVAL;
        break;
    }
    case F_SETFL:                                       /* only these two may change */
        f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) | ((int)arg & (O_NONBLOCK | O_APPEND));
        r = 0;
        break;
    default: r = -EINVAL; break;
    }
    fput(f);
    return r;
}

static int64_t do_dup(int oldfd, int newfd, int flags, bool any)
{
    struct tcb *t = current_task();
    struct file *f = fget(oldfd);
    if (!f) return -EBADF;
    if (any) {
        int n = fd_alloc(t, 0, f, false);               /* our reference goes to the table */
        if (n < 0) fput(f);
        return n;
    }
    if (newfd < 0 || newfd >= MAX_FDS) { fput(f); return -EBADF; }
    if (newfd == oldfd) { fput(f); return flags ? -EINVAL : newfd; }   /* dup3 refuses, dup2 is a no-op */
    uint64_t fl = irq_save();
    struct file *old = t->files->fd[newfd];
    t->files->fd[newfd] = f;
    t->files->cloexec[newfd] = (flags & O_CLOEXEC) != 0;
    irq_restore(fl);
    if (old) vfs_close(old);
    return newfd;
}

static int64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return do_dup((int)fd, 0, 0, true); }

static int64_t sys_dup2(uint64_t fd, uint64_t nfd, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int)fd == (int)nfd) {
        struct file *f = fget((int)fd);
        if (!f) return -EBADF;
        fput(f);
        return (int64_t)nfd;
    }
    return do_dup((int)fd, (int)nfd, 0, false);
}

static int64_t sys_dup3(uint64_t fd, uint64_t nfd, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)O_CLOEXEC) return -EINVAL;
    return do_dup((int)fd, (int)nfd, (int)flags | 1 /* dup3 */ , false);
}

/* Two new descriptors whose numbers go to user memory; if that copy fails
 * both are closed again (Linux does the same). */
static int64_t install_pair(struct file *a, struct file *b, bool cloexec, uint64_t uout)
{
    struct tcb *t = current_task();
    int x = fd_alloc(t, 0, a, cloexec);
    if (x < 0) { vfs_close(a); vfs_close(b); return x; }
    int y = fd_alloc(t, 0, b, cloexec);
    if (y < 0) { vfs_close(fd_take(t, x)); vfs_close(b); return y; }
    int32_t fds[2] = { x, y };
    if (copy_to_user((void *)uout, fds, sizeof fds)) {
        vfs_close(fd_take(t, x));
        vfs_close(fd_take(t, y));
        return -EFAULT;
    }
    return 0;
}

static int64_t sys_pipe2(uint64_t fds, uint64_t flags, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (flags & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
    if (!ubuf_ok(fds, 8)) return -EFAULT;
    struct file *r, *w;
    int rc = pipe_create(&r, &w, (int)flags);
    if (rc) return rc;
    return install_pair(r, w, flags & O_CLOEXEC, fds);
}

static int64_t sys_pipe(uint64_t fds, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; return sys_pipe2(fds, 0, a3, a4, a5, a6); }

struct pollfd { int fd; short events, revents; };

/* Readiness is re-checked every millisecond until something is ready or the
 * timeout expires. The pollfd array is copied in once and the results out
 * once. */
static int64_t do_poll(uint64_t ufds, uint64_t nfds, int64_t tmo)
{
    if (nfds > 4 * MAX_FDS) return -EINVAL;
    struct pollfd *p = nfds ? kmalloc(nfds * sizeof *p) : NULL;
    if (nfds && !p) return -ENOMEM;
    if (copy_from_user(p, (const void *)ufds, nfds * sizeof *p)) { kfree(p); return -EFAULT; }
    uint64_t deadline = tmo > 0 ? time_ms() + (uint64_t)tmo : 0;
    int64_t ready;
    for (;;) {
        ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0) continue;
            struct file *f = fget(p[i].fd);
            p[i].revents = f ? (short)vfs_poll(f, p[i].events | POLLERR | POLLHUP) : POLLNVAL;
            if (f) fput(f);
            if (p[i].revents) ready++;
        }
        if (ready || tmo == 0 || (tmo > 0 && time_ms() >= deadline)) break;
        if (signal_pending()) { ready = -ERESTARTNOHAND; break; }
        task_sleep_ms(1);
    }
    if (ready >= 0 && copy_to_user((void *)ufds, p, nfds * sizeof *p)) ready = -EFAULT;
    kfree(p);
    return ready;
}

static int64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return do_poll(fds, nfds, (int)timeout); }

static int64_t sys_ppoll(uint64_t fds, uint64_t nfds, uint64_t tsp, uint64_t sig, uint64_t a5, uint64_t a6)
{
    (void)sig; (void)a5; (void)a6;
    int64_t tmo = -1;
    if (tsp) {
        struct timespec ts;
        if (copy_from_user(&ts, (const void *)tsp, sizeof ts)) return -EFAULT;
        if (!timespec_ok(&ts)) return -EINVAL;
        tmo = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
    }
    return do_poll(fds, nfds, tmo);
}

/* select(2) and pselect6(2) on top of the same readiness checks as poll:
 * fd_set is a bitmap of 1024 descriptors; only fds below MAX_FDS can be open. */
static int64_t do_select(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, int64_t tmo_ms)
{
    if (n > 1024) return -EINVAL;
    uint64_t bytes = (n + 7) / 8;
    const uint64_t usets[3] = { rp, wp, ep };
    uint8_t in[3][128];
    memset(in, 0, sizeof in);
    for (int k = 0; k < 3; k++) {
        if (!usets[k] || !bytes) continue;
        if (copy_from_user(in[k], (const void *)usets[k], bytes)) return -EFAULT;
        for (uint64_t fd = MAX_FDS; fd < n; fd++)       /* can never be open */
            if (in[k][fd / 8] & (1u << (fd % 8))) return -EBADF;
    }
    uint64_t lim = n < MAX_FDS ? n : MAX_FDS;
    static const int want[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI };
    uint64_t deadline = tmo_ms > 0 ? time_ms() + (uint64_t)tmo_ms : 0;
    for (;;) {
        int ready = 0;
        uint8_t out[3][128];
        memset(out, 0, sizeof out);
        for (uint64_t fd = 0; fd < lim; fd++) {
            for (int k = 0; k < 3; k++) {
                if (!usets[k] || !(in[k][fd / 8] & (1u << (fd % 8)))) continue;
                struct file *f = fget((int)fd);
                if (!f) return -EBADF;
                int ev = vfs_poll(f, want[k]);
                fput(f);
                if (ev & want[k]) {
                    out[k][fd / 8] |= (uint8_t)(1u << (fd % 8));
                    ready++;
                }
            }
        }
        if (ready || tmo_ms == 0 || (tmo_ms > 0 && time_ms() >= deadline)) {
            for (int k = 0; k < 3; k++)
                if (usets[k] && bytes && copy_to_user((void *)usets[k], out[k], bytes)) return -EFAULT;
            return ready;
        }
        if (signal_pending()) return -ERESTARTNOHAND;
        task_sleep_ms(1);
    }
}

static int64_t sys_select(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, uint64_t tvp, uint64_t a6)
{
    (void)a6;
    int64_t tmo = -1;
    if (tvp) {
        int64_t tv[2];                                  /* struct timeval */
        if (copy_from_user(tv, (const void *)tvp, sizeof tv)) return -EFAULT;
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) return -EINVAL;
        tmo = tv[0] * 1000 + (tv[1] + 999) / 1000;
    }
    return do_select(n, rp, wp, ep, tmo);
}

static int64_t sys_pselect6(uint64_t n, uint64_t rp, uint64_t wp, uint64_t ep, uint64_t tsp, uint64_t sig)
{
    (void)sig;
    int64_t tmo = -1;
    if (tsp) {
        struct timespec ts;
        if (copy_from_user(&ts, (const void *)tsp, sizeof ts)) return -EFAULT;
        if (!timespec_ok(&ts)) return -EINVAL;
        tmo = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
    }
    return do_select(n, rp, wp, ep, tmo);
}

/* readlink: there are no symbolic links, except /proc/self/exe and
 * /proc/self/fd/N */
static int64_t do_readlink(const char *p, uint64_t buf, uint64_t len)
{
    if ((int64_t)len <= 0) return -EINVAL;
    struct tcb *t = current_task();
    char out[VFS_PATH_MAX];
    if (!strcmp(p, "/proc/self/exe") && t->exe[0]) {
        strlcpy(out, t->exe, sizeof out);
    } else if (!strncmp(p, "/proc/self/fd/", 14)) {     /* ttyname(): the file behind a descriptor */
        int fd = 0;
        const char *q = p + 14;
        if (!*q) return -ENOENT;
        for (; *q >= '0' && *q <= '9' && fd < MAX_FDS; q++) fd = fd * 10 + (*q - '0');
        if (*q) return -ENOENT;
        struct file *f = fget(fd);
        if (!f) return -ENOENT;
        int r = vfs_path_of(f->vn, out, sizeof out);
        fput(f);
        if (r < 0) return r;
    } else {
        struct stat st;
        int r = vfs_stat(p, &st);
        return r < 0 ? r : -EINVAL;                     /* exists, but is not a link */
    }
    size_t n = strlen(out);
    if (n > len) n = len;
    return copy_to_user((void *)buf, out, n) ? -EFAULT : (int64_t)n;   /* not NUL-terminated, like Linux */
}
static int64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    return r ? r : do_readlink(p, buf, len);
}
static int64_t sys_readlinkat(uint64_t dirfd, uint64_t path, uint64_t buf, uint64_t len, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    return do_readlink(p, buf, len);
}

/* ITIMER_REAL timers: SIGALRM from the scheduler tick (proc/task.c). */
struct ktimeval { int64_t sec, usec; };
static uint64_t tv_ms(const struct ktimeval *tv) { return (uint64_t)tv->sec * 1000 + (uint64_t)(tv->usec + 999) / 1000; }
static void ms_tv(uint64_t ms, struct ktimeval *tv) { tv->sec = (int64_t)(ms / 1000); tv->usec = (int64_t)(ms % 1000) * 1000; }
static void timer_get(struct tcb *t, struct ktimeval out[2])
{
    uint64_t now = time_ms();
    struct signal_struct *g = t->signal;
    ms_tv(g->alarm_every, &out[0]);
    ms_tv(g->alarm_at > now ? g->alarm_at - now : (g->alarm_at ? 1 : 0), &out[1]);
}
static int64_t sys_getitimer(uint64_t which, uint64_t cur, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (which != 0) return -EINVAL;                     /* ITIMER_REAL only */
    struct ktimeval v[2];
    timer_get(current_task(), v);
    return copy_to_user((void *)cur, v, sizeof v) ? -EFAULT : 0;
}
static int64_t sys_setitimer(uint64_t which, uint64_t nv, uint64_t ov, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    if (which != 0) return -EINVAL;
    struct ktimeval v[2], old[2];
    if (nv) {
        if (copy_from_user(v, (const void *)nv, sizeof v)) return -EFAULT;
        if (v[0].sec < 0 || v[1].sec < 0 || v[0].usec < 0 || v[0].usec >= 1000000 || v[1].usec < 0 || v[1].usec >= 1000000)
            return -EINVAL;
    }
    timer_get(t, old);
    if (ov && copy_to_user((void *)ov, old, sizeof old)) return -EFAULT;
    if (nv) {
        uint64_t value = tv_ms(&v[1]);
        t->signal->alarm_every = tv_ms(&v[0]);
        t->signal->alarm_at = value ? time_ms() + value : 0;
    }
    return 0;
}
static int64_t sys_alarm(uint64_t sec, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    uint64_t now = time_ms();
    struct signal_struct *g = t->signal;
    int64_t left = g->alarm_at > now ? (int64_t)((g->alarm_at - now + 999) / 1000) : 0;
    g->alarm_every = 0;
    g->alarm_at = sec ? now + (sec > (1ull << 32) ? (1ull << 32) : sec) * 1000 : 0;
    return left;
}

/* prctl: process names (busybox renames its no-exec applets), and a few
 * options that are accepted without effect. */
static int64_t sys_prctl(uint64_t op, uint64_t arg, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    switch (op) {
    case 15: {                                          /* PR_SET_NAME */
        char name[16];
        long n = strncpy_from_user(name, (const char *)arg, sizeof name);
        if (n == -EFAULT) return -EFAULT;
        name[sizeof name - 1] = 0;                      /* longer names are cut, like Linux */
        strlcpy(t->name, name, sizeof t->name);
        return 0;
    }
    case 16: {                                          /* PR_GET_NAME */
        char name[16] = { 0 };
        strlcpy(name, t->name, sizeof name);
        return copy_to_user((void *)arg, name, sizeof name) ? -EFAULT : 0;
    }
    case 1: return 0;                                   /* PR_SET_PDEATHSIG: never sent */
    case 3: return 1;                                   /* PR_GET_DUMPABLE */
    case 4: return 0;                                   /* PR_SET_DUMPABLE */
    default: return -EINVAL;
    }
}

/* times/getrusage: a process's CPU time is all counted as user time
 * (system calls included); children's times are not accumulated. */
static int64_t sys_times(uint64_t p, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (p) {
        uint64_t tms[4] = { current_task()->cpu_ticks * 100 / PIT_HZ, 0, 0, 0 };   /* USER_HZ */
        if (copy_to_user((void *)p, tms, sizeof tms)) return -EFAULT;
    }
    return (int64_t)(time_ms() / 10);
}
static int64_t sys_getrusage(uint64_t who, uint64_t p, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if ((int64_t)who != 0 && (int64_t)who != -1 && (int64_t)who != 1) return -EINVAL;   /* SELF, CHILDREN, THREAD */
    int64_t ru[18];
    memset(ru, 0, sizeof ru);
    if ((int64_t)who != -1) {
        uint64_t us = current_task()->cpu_ticks * (1000000 / PIT_HZ);
        ru[0] = (int64_t)(us / 1000000);                /* ru_utime */
        ru[1] = (int64_t)(us % 1000000);
    }
    return copy_to_user((void *)p, ru, sizeof ru) ? -EFAULT : 0;
}

/* sysinfo(2), Linux x86_64 layout */
struct ksysinfo {
    int64_t  uptime;
    uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char     reserved[4];
};
static int64_t sys_sysinfo(uint64_t p, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct ksysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime = (int64_t)(time_ms() / 1000);
    si.totalram = pmm_total_bytes();
    si.freeram = pmm_free_bytes();
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (task_slot(i)->state != TASK_UNUSED) n++;
    si.procs = (uint16_t)n;
    si.mem_unit = 1;
    return copy_to_user((void *)p, &si, sizeof si) ? -EFAULT : 0;
}

static int64_t sys_access(uint64_t path, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (mode & ~7ull) return -EINVAL;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    struct stat st;
    return vfs_stat(p, &st);                            /* existence; everyone is root */
}

static int64_t sys_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    char p[VFS_PATH_MAX];
    int r = get_path(path, p);
    if (r) return r;
    if ((int)dirfd != AT_FDCWD && p[0] != '/') return -ENOSYS;
    return sys_access(path, mode, 0, 0, 0, 0);
}

static int64_t sys_umask(uint64_t mask, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_task();
    uint32_t old = t->fs->umask;
    t->fs->umask = (uint32_t)mask & 0777;
    return old;
}

/* RDRAND when the CPU has it, else a TSC-seeded xorshift (fine for X auth
 * cookies on a hobby OS; not for cryptography). Generated in kernel chunks
 * and copied out. */
static int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a4; (void)a5; (void)a6;
    if (len > 1 << 20) len = 1 << 20;
    if (!ubuf_ok(buf, len)) return -EFAULT;
    static int has_rdrand = -1;
    static uint64_t x;
    if (has_rdrand < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        has_rdrand = (c >> 30) & 1;
        x = rdtsc() | 1;
    }
    uint8_t chunk[256];
    for (uint64_t done = 0; done < len; ) {
        size_t n = len - done < sizeof chunk ? (size_t)(len - done) : sizeof chunk;
        for (size_t i = 0; i < n; i += 8) {
            uint64_t v = 0;
            unsigned char ok = 0;
            if (has_rdrand) __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (!ok) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v = x ^ rdtsc(); }
            for (size_t k = 0; k < 8 && i + k < n; k++) chunk[i + k] = (uint8_t)(v >> (8 * k));
        }
        if (copy_to_user((void *)(buf + done), chunk, n)) return done ? (int64_t)done : -EFAULT;
        done += n;
    }
    return (int64_t)len;
}

/* ---- AF_UNIX sockets (fs/unixsock.c) ------------------------------------ */
/* The socket behind fd, with a reference (fput it). */
static int64_t sock_get(int fd, struct file **out)
{
    struct file *f = fget(fd);
    if (!f) return -EBADF;
    if (!usock_is(f)) { fput(f); return -ENOTSOCK; }
    *out = f;
    return 0;
}

/* A socket address from user memory: bounded and copied whole. */
static int get_sockaddr(uint64_t uaddr, uint64_t len, struct sockaddr_un *a)
{
    if (len > sizeof *a) return -EINVAL;
    memset(a, 0, sizeof *a);
    return copy_from_user(a, (const void *)uaddr, (size_t)len) ? -EFAULT : 0;
}

/* Store an address for accept/getsockname/recvfrom: *ulenp in, real length out. */
static int put_sockaddr(uint64_t uaddr, uint64_t ulenp, const struct sockaddr_un *a, uint32_t got)
{
    uint32_t cap;
    if (get_user_u32(&cap, (const void *)ulenp)) return -EFAULT;
    if ((int32_t)cap < 0) return -EINVAL;
    if (cap && copy_to_user((void *)uaddr, a, got < cap ? got : cap)) return -EFAULT;
    return put_user_u32((void *)ulenp, got);
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
    if (!ubuf_ok(sv, 8)) return -EFAULT;
    struct file *a, *b;
    int rc = usock_pair((int)type, &a, &b);
    if (rc) return rc;
    return install_pair(a, b, type & SOCK_CLOEXEC, sv);
}

static int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct sockaddr_un a;
    int64_t rc = get_sockaddr(addr, len, &a);
    if (rc) return rc;
    struct file *f;
    if ((rc = sock_get((int)fd, &f))) return rc;
    rc = usock_bind(f, &a, (uint32_t)len);
    fput(f);
    return rc;
}

static int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    rc = usock_listen(f, (int)backlog);
    fput(f);
    return rc;
}

static int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    struct sockaddr_un a;
    int64_t rc = get_sockaddr(addr, len, &a);
    if (rc) return rc;
    struct file *f;
    if ((rc = sock_get((int)fd, &f))) return rc;
    rc = usock_connect(f, &a, (uint32_t)len);
    fput(f);
    return rc;
}

static int64_t sys_accept4(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    struct file *nf;
    struct sockaddr_un a;
    uint32_t got = sizeof a;
    rc = usock_accept(f, (int)flags, &nf, &a, &got);
    fput(f);
    if (rc) return rc;
    if (addr && (rc = put_sockaddr(addr, lenp, &a, got))) { vfs_close(nf); return rc; }
    return install(nf, flags & SOCK_CLOEXEC);
}

static int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; return sys_accept4(fd, addr, lenp, 0, a5, a6); }

/* Socket data through a bounce buffer: one send/receive per chunk, a stream
 * send continues until all is queued (or it would block, with MSG_DONTWAIT
 * after the first chunk). */
static int64_t sock_xfer(struct file *f, uint64_t ubuf, uint64_t len, int flags, bool send)
{
    if (!len) return send ? usock_send(f, "", 0, flags) : usock_recv(f, NULL, 0, flags);
    if (!ubuf_ok(ubuf, len)) return -EFAULT;
    size_t chunk = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
    uint8_t *kb = kmalloc(chunk);
    if (!kb) return -ENOMEM;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? (size_t)(len - (uint64_t)total) : chunk;
        long n;
        if (send) {
            if (copy_from_user(kb, (const void *)(ubuf + (uint64_t)total), want)) { if (!total) total = -EFAULT; break; }
            n = usock_send(f, kb, want, flags | (total ? MSG_DONTWAIT : 0));
        } else {
            n = usock_recv(f, kb, want, flags | (total ? MSG_DONTWAIT : 0));
            if (n > 0 && copy_to_user((void *)(ubuf + (uint64_t)total), kb, (size_t)n)) { if (!total) total = -EFAULT; break; }
        }
        if (n < 0) { if (!total) total = n; break; }
        total += n;
        if ((size_t)n < want || !send) break;           /* a receive returns what is there */
    }
    kfree(kb);
    return total;
}

static int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alen)
{
    (void)alen;
    if ((int64_t)len < 0) return -EINVAL;
    if (addr) return -EISCONN;                          /* stream sockets only */
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    rc = sock_xfer(f, buf, len, (int)flags, true);
    fput(f);
    return rc;
}

static int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t alenp)
{
    if ((int64_t)len < 0) return -EINVAL;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    rc = sock_xfer(f, buf, len, (int)flags, false);
    fput(f);
    if (rc >= 0 && addr && alenp && put_user_u32((void *)alenp, 0)) return -EFAULT;
    return rc;
}

struct msghdr_k {
    uint64_t name; uint32_t namelen, pad0;
    uint64_t iov, iovlen, control, controllen;
    int32_t  flags, pad1;
};

/* Data only for now: a message carrying ancillary data (SCM_RIGHTS
 * descriptor passing) is refused, never dropped. */
static int64_t msg_io(uint64_t fd, uint64_t umsg, uint64_t flags, bool send)
{
    struct msghdr_k m;
    if (copy_from_user(&m, (const void *)umsg, sizeof m)) return -EFAULT;
    if (send && m.controllen) return -EOPNOTSUPP;
    if (m.iovlen > 1024) return -EMSGSIZE;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    struct iovec *v = m.iovlen ? kmalloc(m.iovlen * sizeof *v) : NULL;
    int64_t total = 0;
    if (m.iovlen && !v) { total = -ENOMEM; goto out; }
    if (copy_from_user(v, (const void *)m.iov, m.iovlen * sizeof *v)) { total = -EFAULT; goto out; }
    for (uint64_t i = 0; i < m.iovlen; i++) {
        if (!v[i].len) continue;
        if ((int64_t)v[i].len < 0) { if (!total) total = -EINVAL; break; }
        int64_t n = sock_xfer(f, v[i].base, v[i].len, (int)flags | (total ? MSG_DONTWAIT : 0), send);
        if (n < 0) { if (!total) total = n; break; }
        total += n;
        if ((uint64_t)n < v[i].len) break;
    }
    if (!send && total >= 0) {
        m.controllen = 0; m.flags = 0;
        if (m.name) m.namelen = 0;
        if (copy_to_user((void *)umsg, &m, sizeof m)) total = -EFAULT;
    }
out:
    kfree(v);
    fput(f);
    return total;
}

static int64_t sys_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return msg_io(fd, msg, flags, true); }

static int64_t sys_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return msg_io(fd, msg, flags, false); }

static int64_t sys_shutdown(uint64_t fd, uint64_t how, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (how > SHUT_RDWR) return -EINVAL;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    rc = usock_shutdown(f, (int)how);
    fput(f);
    return rc;
}

static int64_t sock_name(uint64_t fd, uint64_t addr, uint64_t lenp, bool peer)
{
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    struct sockaddr_un tmp;
    uint32_t got = sizeof tmp;
    rc = usock_name(f, peer, &tmp, &got);
    fput(f);
    return rc ? rc : put_sockaddr(addr, lenp, &tmp, got);
}

static int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return sock_name(fd, addr, lenp, false); }

static int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t lenp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return sock_name(fd, addr, lenp, true); }

/* Options are accepted and ignored (SO_REUSEADDR, buffer sizes, ...). */
static int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t len, uint64_t a6)
{
    (void)level; (void)opt; (void)val; (void)a6;
    if ((int32_t)len < 0) return -EINVAL;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    fput(f);
    return 0;
}

static int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t lenp, uint64_t a6)
{
    (void)a6;
    uint32_t len;
    if (get_user_u32(&len, (const void *)lenp)) return -EFAULT;
    if ((int32_t)len < 0) return -EINVAL;
    uint8_t kv[64];
    memset(kv, 0, sizeof kv);
    uint32_t klen = len < sizeof kv ? len : sizeof kv;
    struct file *f; int64_t rc = sock_get((int)fd, &f);
    if (rc) return rc;
    rc = usock_getsockopt(f, (int)level, (int)opt, kv, &klen);
    fput(f);
    if (rc) return rc;
    if (copy_to_user((void *)val, kv, klen)) return -EFAULT;
    return put_user_u32((void *)lenp, klen);
}

/* sendfile(out, in, offset, count): copy through a kernel buffer. With an
 * offset pointer the input is read from there and its file position kept. */
static int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offp, uint64_t count, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    struct file *in = fget((int)in_fd), *out = fget((int)out_fd);
    int64_t total = 0;
    uint8_t *buf = NULL;
    off_t saved = 0, start = 0;
    if (!in || !out) { total = -EBADF; goto done; }
    if (offp) {
        uint64_t o;
        if (get_user_u64(&o, (const void *)offp)) { total = -EFAULT; goto done; }
        if ((int64_t)o < 0) { total = -EINVAL; goto done; }
        start = (off_t)o;
        saved = vfs_lseek(in, 0, 1);                    /* SEEK_CUR */
        if (saved < 0 || vfs_lseek(in, start, 0) < 0) { total = -ESPIPE; goto done; }
    }
    buf = kmalloc(IO_CHUNK);
    if (!buf) { total = -ENOMEM; goto restore; }
    while ((uint64_t)total < count) {
        size_t want = count - (uint64_t)total < IO_CHUNK ? (size_t)(count - (uint64_t)total) : IO_CHUNK;
        ssize_t n = vfs_read(in, buf, want);
        if (n <= 0) { if (!total && n < 0) total = n; break; }
        ssize_t w = vfs_write(out, buf, (size_t)n);
        if (w < 0) { if (!total) total = w; break; }
        total += w;
        if (w < n) break;
    }
restore:
    if (offp) {
        vfs_lseek(in, saved, 0);
        if (total > 0 && put_user_u64((void *)offp, (uint64_t)(start + total))) total = -EFAULT;
    }
done:
    kfree(buf);
    if (in) fput(in);
    if (out) fput(out);
    return total;
}

/* ---- user and group ids: one uid and one gid per process (real = effective
 * = saved); root may switch to any id, everyone else only to their own ---- */
static int64_t set_id(uint32_t *field, int64_t want)
{
    struct tcb *t = current_task();
    if (want == -1) return 0;                           /* "unchanged" */
    if (want < 0) return -EINVAL;
    if (t->uid != 0 && (uint32_t)want != *field) return -EPERM;
    *field = (uint32_t)want;
    return 0;
}
static int64_t sys_setuid(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return set_id(&current_task()->uid, (int64_t)(int32_t)id); }
static int64_t sys_setgid(uint64_t id, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return set_id(&current_task()->gid, (int64_t)(int32_t)id); }
/* setreuid/setresuid (and the gid variants): with one id per process, the
 * effective id is the one that counts; the others must agree or be -1. */
static int64_t set_ids(uint32_t *field, int64_t r, int64_t e, int64_t s)
{
    int64_t want = e != -1 ? e : r != -1 ? r : s;
    if ((r != -1 && r != want) || (s != -1 && s != want)) {
        if (current_task()->uid != 0) return -EPERM;
    }
    return set_id(field, want);
}
static int64_t sys_setreuid(uint64_t r, uint64_t e, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return set_ids(&current_task()->uid, (int32_t)r, (int32_t)e, -1); }
static int64_t sys_setregid(uint64_t r, uint64_t e, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return set_ids(&current_task()->gid, (int32_t)r, (int32_t)e, -1); }
static int64_t sys_setresuid(uint64_t r, uint64_t e, uint64_t sv, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return set_ids(&current_task()->uid, (int32_t)r, (int32_t)e, (int32_t)sv); }
static int64_t sys_setresgid(uint64_t r, uint64_t e, uint64_t sv, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return set_ids(&current_task()->gid, (int32_t)r, (int32_t)e, (int32_t)sv); }
static int64_t get_ids(uint32_t v, uint64_t r, uint64_t e, uint64_t sv)
{
    if (put_user_u32((void *)r, v) || put_user_u32((void *)e, v) || put_user_u32((void *)sv, v)) return -EFAULT;
    return 0;
}
static int64_t sys_getresuid(uint64_t r, uint64_t e, uint64_t sv, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return get_ids(current_task()->uid, r, e, sv); }
static int64_t sys_getresgid(uint64_t r, uint64_t e, uint64_t sv, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return get_ids(current_task()->gid, r, e, sv); }
static int64_t sys_getgroups(uint64_t n, uint64_t list, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)list; (void)a3; (void)a4; (void)a5; (void)a6; return (int32_t)n < 0 ? -EINVAL : 0; }   /* no supplementary groups */
static int64_t sys_setgroups(uint64_t n, uint64_t list, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)list; (void)a3; (void)a4; (void)a5; (void)a6; return current_task()->uid == 0 || n == 0 ? 0 : -EPERM; }

/* ---- resource limits: fixed by the kernel, reported Linux-style ------------ */
#define RLIM_INF (~0ull)
static void rlimit_of(int res, uint64_t out[2])
{
    switch (res) {
    case 3:  out[0] = out[1] = UVM_STACK_SIZE; break;    /* RLIMIT_STACK  */
    case 7:  out[0] = out[1] = MAX_FDS; break;           /* RLIMIT_NOFILE */
    case 6:  out[0] = out[1] = MAX_TASKS; break;         /* RLIMIT_NPROC  */
    case 4:  out[0] = out[1] = 0; break;                 /* RLIMIT_CORE   */
    default: out[0] = out[1] = RLIM_INF; break;
    }
}

static int64_t sys_prlimit64(uint64_t pid, uint64_t res, uint64_t newp, uint64_t oldp, uint64_t a5, uint64_t a6)
{
    (void)a5; (void)a6;
    if (pid && (int)pid != current_task()->tgid) return -EPERM;
    if (res > 15) return -EINVAL;
    uint64_t cur[2], n[2];
    rlimit_of((int)res, cur);
    if (newp) {                                         /* lowering is accepted, raising is not */
        if (copy_from_user(n, (const void *)newp, sizeof n)) return -EFAULT;
        if (n[0] > n[1] || n[1] > cur[1]) return -EPERM;
    }
    if (oldp && copy_to_user((void *)oldp, cur, sizeof cur)) return -EFAULT;
    return 0;
}
static int64_t sys_getrlimit(uint64_t res, uint64_t oldp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sys_prlimit64(0, res, 0, oldp, 0, 0); }
static int64_t sys_setrlimit(uint64_t res, uint64_t newp, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sys_prlimit64(0, res, newp, 0, 0, 0); }

/* ---- processes and signals (proc/process.c) ---------------------------- */
#define SIGCHLD_ 17
#define CLONE_VM_ 0x100
#define CLONE_VFORK_ 0x4000
static int64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_clone(SIGCHLD_, 0, 0, 0, 0); }
static int64_t sys_vfork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_clone(CLONE_VM_ | CLONE_VFORK_ | SIGCHLD_, 0, 0, 0, 0); }
static int64_t sys_clone(uint64_t flags, uint64_t sp, uint64_t ptid, uint64_t ctid, uint64_t tls, uint64_t a6)
{ (void)a6; return proc_clone(flags, sp, ptid, ctid, tls); }
static int64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return proc_execve(path, argv, envp); }
static int64_t sys_wait4(uint64_t pid, uint64_t status, uint64_t options, uint64_t ru, uint64_t a5, uint64_t a6)
{ (void)a5; (void)a6; return proc_wait4((int64_t)(int32_t)pid, status, options, ru); }
static int64_t sys_setsid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_setsid(); }
static int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return proc_setpgid((int64_t)(int32_t)pid, (int64_t)(int32_t)pgid); }
static int64_t sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_getpgid((int64_t)(int32_t)pid); }
static int64_t sys_getpgrp(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_getpgid(0); }
static int64_t sys_getsid(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return proc_getsid((int64_t)(int32_t)pid); }
static int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t old, uint64_t size, uint64_t a5, uint64_t a6)
{ (void)a5; (void)a6; return sig_action(sig, act, old, size); }
static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t old, uint64_t size, uint64_t a5, uint64_t a6)
{ (void)a5; (void)a6; return sig_procmask(how, set, old, size); }
static int64_t sys_rt_sigpending(uint64_t set, uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sig_pending_set(set, size); }
static int64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return sig_return(); }
static int64_t sys_rt_sigsuspend(uint64_t mask, uint64_t size, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sig_suspend(mask, size); }
static int64_t sys_pause(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; return sig_pause(); }
static int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sig_kill((int64_t)(int32_t)pid, (int64_t)(int32_t)sig); }
static int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a3; (void)a4; (void)a5; (void)a6; return sig_tgkill(0, (int64_t)(int32_t)tid, (int64_t)(int32_t)sig); }
static int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a4; (void)a5; (void)a6; return sig_tgkill((int64_t)(int32_t)tgid, (int64_t)(int32_t)tid, (int64_t)(int32_t)sig); }

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
    REG(SYS_exit, sys_exit);           REG(SYS_exit_group, sys_exit_group);
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
    REG(SYS_futex, sys_futex);
    REG(SYS_set_robust_list, sys_set_robust_list);
    REG(SYS_get_robust_list, sys_get_robust_list);
    REG(SYS_sched_getaffinity, sys_sched_getaffinity);
    REG(SYS_sched_setaffinity, sys_sched_setaffinity);
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
    REG(SYS_fork, sys_fork);           REG(SYS_execve, sys_execve);
    REG(SYS_vfork, sys_vfork);         REG(SYS_clone, sys_clone);
    REG(SYS_setsid, sys_setsid);       REG(SYS_setpgid, sys_setpgid);
    REG(SYS_getpgrp, sys_getpgrp);     REG(SYS_getsid, sys_getsid);
    REG(SYS_rt_sigreturn, sys_rt_sigreturn); REG(SYS_rt_sigpending, sys_rt_sigpending);
    REG(SYS_rt_sigsuspend, sys_rt_sigsuspend); REG(SYS_pause, sys_pause);
    REG(SYS_tkill, sys_tkill);         REG(SYS_tgkill, sys_tgkill);
    REG(SYS_getrlimit, sys_getrlimit); REG(SYS_setrlimit, sys_setrlimit);
    REG(SYS_prlimit64, sys_prlimit64); REG(SYS_sendfile, sys_sendfile);
    REG(SYS_setuid, sys_setuid);       REG(SYS_setgid, sys_setgid);
    REG(SYS_setreuid, sys_setreuid);   REG(SYS_setregid, sys_setregid);
    REG(SYS_setresuid, sys_setresuid); REG(SYS_setresgid, sys_setresgid);
    REG(SYS_getresuid, sys_getresuid); REG(SYS_getresgid, sys_getresgid);
    REG(SYS_getgroups, sys_getgroups); REG(SYS_setgroups, sys_setgroups);
    REG(SYS_readlink, sys_readlink);   REG(SYS_readlinkat, sys_readlinkat);
    REG(SYS_getitimer, sys_getitimer); REG(SYS_setitimer, sys_setitimer);
    REG(SYS_alarm, sys_alarm);         REG(SYS_sysinfo, sys_sysinfo);
    REG(SYS_prctl, sys_prctl);         REG(SYS_times, sys_times);
    REG(SYS_getrusage, sys_getrusage);
    REG(SYS_wait4, sys_wait4);        REG(SYS_kill, sys_kill);
    REG(SYS_gettimeofday, sys_gettimeofday);
    REG(SYS_pread64, sys_pread64);     REG(SYS_pwrite64, sys_pwrite64);
    REG(SYS_openat, sys_openat);       REG(SYS_newfstatat, sys_newfstatat);
    REG(SYS_unlinkat, sys_unlinkat);   REG(SYS_mkdirat, sys_mkdirat);
    REG(SYS_faccessat, sys_faccessat); REG(SYS_ppoll, sys_ppoll);
    REG(SYS_mremap, sys_mremap);       REG(SYS_msync, sys_msync);
    REG(SYS_mincore, sys_mincore);     REG(SYS_ftruncate, sys_ftruncate);
    REG(SYS_truncate, sys_truncate);   REG(SYS_fsync, sys_fsync);
    REG(SYS_fdatasync, sys_fsync);     REG(SYS_memfd_create, sys_memfd_create);

    /* SYSCALL/SYSRET fast path (used once ring-3 processes exist) */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);                       /* EFER.SCE */
    wrmsr(MSR_STAR, ((uint64_t)(GDT_USER_DATA - 8) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    /* SFMASK: RFLAGS bits cleared on entry. IF off until the kernel stack is
     * in place; DF, TF, AC and NT never leak from user space into the kernel. */
    wrmsr(MSR_FMASK, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000);

    int n = 0;
    for (int i = 0; i < SYS_MAX; i++) if (table[i] && table[i] != sys_enosys) n++;
    kprintf("syscall: int 0x80 gate + SYSCALL/LSTAR ready, %d calls implemented\n", n);
}

int syscall_dispatch(struct int_frame *f)
{
    uint64_t nr = f->rax;
    struct tcb *t = current_task();
    if (nr >= SYS_MAX || !table[nr] || table[nr] == sys_enosys) {
        static uint8_t warned[SYS_MAX + 1];
        uint64_t k = nr < SYS_MAX ? nr : SYS_MAX;
        if (t->user && warned[k] < 3) {         /* the gaps a ported program hits */
            warned[k]++;
            kprintf("syscall: pid %d (%s) called unimplemented syscall %lu (%s) at %lx\n", t->pid, t->name, nr,
                    nr < SYS_MAX && names[nr] ? names[nr] : "?", f->rip);
        }
        f->rax = (uint64_t)-ENOSYS;
    } else {
        counts[nr]++;
        t->uframe = f;
        t->iret_return = false;
        if (f->rflags & (1u << 9)) sti();   /* preemptible if the caller was */
        int64_t r = table[nr](f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
        f->rax = (uint64_t)r;
        /* a write to a pipe or socket nobody reads raises SIGPIPE */
        if (r == -EPIPE && t->user &&
            (nr == SYS_write || nr == SYS_writev ||
             (nr == SYS_sendto && !(f->r10 & MSG_NOSIGNAL)) || (nr == SYS_sendmsg && !(f->rdx & MSG_NOSIGNAL))))
            signal_send_thread(t, SIGPIPE, NULL);
        cli();
    }
    if (t->user && (f->cs & 3) == 3) signal_deliver(f, nr, true);
    int iret = t->iret_return;
    t->iret_return = false;
    return iret;
}

const char *syscall_name(int nr) { return (nr >= 0 && nr < SYS_MAX && names[nr]) ? names[nr] : NULL; }
int syscall_implemented(int nr) { return nr >= 0 && nr < SYS_MAX && table[nr] && table[nr] != sys_enosys; }
uint64_t syscall_count(int nr) { return (nr >= 0 && nr < SYS_MAX) ? counts[nr] : 0; }
