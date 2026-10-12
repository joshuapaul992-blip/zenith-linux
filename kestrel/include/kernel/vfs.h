/* include/kernel/vfs.h -- virtual file system layer
 *
 *   /            ramfs   (read-write, in memory)
 *   /dev         devfs   (character devices: null zero random tty console fb0 kmsg ttyS0)
 *   /proc        procfs  (generated: version uptime meminfo cpuinfo mounts tasks ...)
 *   /sys         sysfs   (kernel objects: class/graphics/fb0, kernel, firmware, devices)
 *   /tmp /etc /bin /home ...   plain ramfs directories
 *
 * Every filesystem shares one in-memory node structure (struct vnode).
 * Filesystems differ only in their vnode_ops and mount flags. A mount
 * point is a directory vnode whose `mounted` field points at the root of
 * another filesystem; path walking transparently crosses it. */
#ifndef KESTREL_VFS_H
#define KESTREL_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/posix.h>
#include <kernel/kmutex.h>

#define VFS_NAME_MAX 256              /* a name, with its NUL (Linux NAME_MAX 255) */
#define VFS_PATH_MAX 1024             /* a path given to a call, with its NUL */
#define VFS_MAX_SYMLINKS 40           /* links followed in one lookup (ELOOP) */

enum vtype { VNON = 0, VREG, VDIR, VCHR, VSOCK, VLNK };

struct vnode;
struct mount;
struct file;

struct vnode_ops {
    ssize_t (*read)(struct vnode *vn, void *buf, size_t len, uint64_t off);
    ssize_t (*write)(struct vnode *vn, const void *buf, size_t len, uint64_t off);
    int     (*ioctl)(struct vnode *vn, unsigned long req, void *arg);
    uint64_t (*size)(struct vnode *vn);         /* optional dynamic size */
    /* Optional, for objects whose behaviour depends on the open file
     * (O_NONBLOCK) or that need to know when a file closes: pipes, sockets. */
    ssize_t (*fread)(struct file *f, void *buf, size_t len);
    ssize_t (*fwrite)(struct file *f, const void *buf, size_t len);
    int     (*poll)(struct file *f, int events);   /* ready POLL* bits     */
    /* fread, fwrite, poll and poll_deadline lock the object themselves:
     * reads, writes and polls of the file run without the big kernel lock
     * (kernel/syscall.c: fget). Pipes, eventfds and unix sockets. On a file
     * system's nodes it says that every operation locks itself, so the path
     * calls (stat, readlink, open) call them without the big kernel lock. */
    bool    nolock;
    void    (*release)(struct file *f);            /* last close of f      */
    int     (*fioctl)(struct file *f, unsigned long req, void *arg);  /* preferred over ioctl */
    /* Optional: create the open file (devices that hand out a new object per
     * open, e.g. /dev/ptmx). Called after vfs_open's permission checks. */
    int     (*open)(struct vnode *vn, int flags, struct file **out);
    /* Optional, directories: bring the children up to date before a lookup
     * or listing (a disk directory read when first used, /proc's
     * per-process directories). Called with the tree's lock held shared, so
     * it may only append children (vfs_node_add), and must keep any other
     * task from appending to the same directory meanwhile -- unless `stale`
     * is set: then refresh runs with the lock held exclusively and may also
     * take children away, and `stale` (called with it shared) says whether
     * a refresh is needed at all. */
    void    (*refresh)(struct vnode *dir);
    bool    (*stale)(struct vnode *dir);
    /* Optional, regular files: set the size (ftruncate, O_TRUNC). */
    int     (*truncate)(struct vnode *vn, uint64_t size);
    /* Optional: the page-backed object holding the file's data, so that
     * MAP_SHARED mappings and read()/write() see the same bytes (ramfs,
     * memfd). NULL: no shared mappings of this file. */
    struct vm_object *(*vmobject)(struct vnode *vn);
    /* Optional, regular files of a disk file system: the page cache's
     * frame holding the file's bytes [idx * 4096, +4096), zero past the
     * end, with a reference for the caller (page_unref when done). With
     * `io` false only a page already cached (*phys = 0 if none). Private
     * mappings map these frames instead of copying (pagecache.h). */
    int     (*getpage)(struct vnode *vn, uint64_t idx, bool io, uint64_t *phys);
    /* Optional: the time_ms() at which readiness may change on its own
     * (timerfd expiry), so poll/epoll sleep exactly that long. 0: never. */
    uint64_t (*poll_deadline)(struct file *f);
    /* Symbolic links: copy the target (no NUL) into buf, return its length. */
    int     (*readlink)(struct vnode *vn, char *buf, size_t size);
    /* A disk file system's directory operations (on the directory's ops).
     * Without them the in-memory tree is the whole truth (ramfs). The VFS
     * has checked names, types, emptiness and write access; on success it
     * updates the in-memory tree itself. */
    int     (*create)(struct vnode *dir, const char *name, enum vtype t, uint32_t mode, const char *target,
                      struct vnode **out);      /* file, directory or symlink (target) */
    int     (*link)(struct vnode *dir, const char *name, struct vnode *target, struct vnode **out);
    int     (*remove)(struct vnode *dir, struct vnode *vn);         /* unlink / rmdir */
    int     (*rename)(struct vnode *odir, struct vnode *vn, struct vnode *ndir, const char *nname,
                      struct vnode *replaced);  /* replaced: the entry overwritten, or NULL */
    int     (*setattr)(struct vnode *vn);       /* mode, uid, gid, times changed: write back */
    void    (*evict)(struct vnode *vn);         /* the vnode is being freed */
    void    (*getattr)(struct vnode *vn);       /* refresh size, nlink, times (before stat) */
};

/* poll(2) bits */
#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLWRNORM  0x100
#define POLLRDHUP   0x2000

struct vnode {
    char        name[VFS_NAME_MAX];
    enum vtype  type;
    uint32_t    mode;               /* S_IF* | permission bits            */
    uint64_t    ino;
    uint64_t    size;
    uint32_t    uid, gid;
    uint32_t    rdev;               /* (major << 8) | minor for devices   */
    uint64_t    mtime;
    const struct vnode_ops *ops;
    void       *data;               /* fs private: buffer, generator, ... */
    void       *ctx;                /* fs private: extra argument         */
    size_t      cap;                /* ramfs: allocated buffer size       */
    struct mount *fs;
    struct vnode *parent, *children, *sibling;
    struct vnode *mounted;          /* root of filesystem mounted here    */
    int         refs;               /* open files, lookups in progress and
                                       working directories on it (atomic);
                                       fs/vfs.c: VN_GONE once out of the tree */
    bool        unlinked;           /* removed from its directory: freed
                                       when the last reference goes        */
    bool        loaded;             /* directory: children read (refresh)  */
    bool        loading;            /* ...being read: by the holder of the fs lock */
    uint32_t    nlink;              /* hard links (0: 1, or 2 for a dir)   */
    uint64_t    atime, ctime;       /* seconds (0: same as mtime)          */
    uint64_t    blocks;             /* 512-byte blocks used (0: from size) */
    uint64_t    dpos;               /* its getdents offset in its directory */
    uint64_t    dnext;              /* directory: the next child's dpos     */
};

#define MNT_RDONLY   0x1
#define MNT_NOCREATE 0x2            /* pseudo fs: structure fixed by kernel */

/* statfs(2), Linux x86_64 layout */
struct kstatfs {
    int64_t  f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen, f_frsize, f_flags, f_spare[4];
};

struct mount {
    char          path[VFS_PATH_MAX];
    char          fstype[16];
    struct vnode *root;
    struct vnode *covered;
    uint32_t      flags;
    uint32_t      dev;
    struct mount *next;
    /* a disk file system's own state and whole-volume operations */
    void         *priv;
    int         (*sync)(struct mount *m, bool final);           /* write back everything; final: shutting down */
    int         (*statfs)(struct mount *m, struct kstatfs *st);
};

struct epitem;
/* The `off` a write op gets for an O_APPEND write of a regular file: the
 * file's end, which the file system reads under its own lock. */
#define VFS_APPEND UINT64_MAX

struct file {
    struct vnode *vn;
    uint64_t      off;
    struct kmutex pos_lock;         /* off, across a read or write of a regular file */
    int           flags;
    int           refcnt;
    struct epitem *ep_links;        /* epoll sets watching this file (fs/epoll.c) */
};

/* ---- core ------------------------------------------------------------- */
void          vfs_init(void);
struct vnode *vfs_root(void);
struct mount *vfs_mounts(void);
struct vnode *vfs_node_new(struct mount *fs, const char *name, enum vtype t, uint32_t mode,
                           const struct vnode_ops *ops);
/* Append to a directory: at boot, or with the tree's lock held exclusively
 * (or shared, from a refresh: see vnode_ops.refresh). So do pseudo_dir and
 * pseudo_file below. */
void          vfs_node_add(struct vnode *dir, struct vnode *child);
struct vnode *vfs_mkfs(const char *fstype, uint32_t flags, struct mount **out);
int           vfs_mount(struct mount *m, const char *path);
int           vfs_bind(const char *src, const char *dst);     /* show src's contents at dst */

/* ---- the tree's lock and node references ------------------------------------
 * One reader-writer lock guards the directory tree (fs/vfs.c): lookups hold
 * it shared, creating, removing and renaming exclusively; the path calls
 * run without the big kernel lock. A node found by a lookup carries a
 * reference, so it stays valid after the lock is dropped even if another
 * CPU unlinks it meanwhile. */
struct fs_struct;
void    vfs_tree_lock(bool excl);               /* recursive per task */
void    vfs_tree_unlock(void);
void    vfs_vnode_get(struct vnode *vn);        /* NULL is fine */
void    vfs_vnode_put(struct vnode *vn);        /* frees an unlinked node with its last reference */
struct vnode *vfs_cwd_get(struct fs_struct *fs);   /* fs's working directory (root if none), referenced */
void    vfs_set_cwd(struct vnode *vn);          /* the calling task's; takes its own reference */

/* ---- path operations ------------------------------------------------------
 * Relative paths start at `base` (a directory, the *at calls' dirfd), or the
 * current task's cwd when base is NULL. Symbolic links are followed, at most
 * VFS_MAX_SYMLINKS per lookup, except in the last component when `follow`
 * is false (lstat, unlink, O_NOFOLLOW ...). The lookups return the node
 * with a reference: vfs_vnode_put() when done. */
int     vfs_lookup(const char *path, struct vnode **out);
int     vfs_lookup_at(struct vnode *base, const char *path, bool follow, struct vnode **out);
int     vfs_open_at(struct vnode *base, const char *path, int flags, mode_t mode, struct file **out);
int     vfs_open_vnode(struct vnode *vn, int flags, struct file **out);
int     vfs_mkdir_at(struct vnode *base, const char *path, mode_t mode);
int     vfs_unlink_at(struct vnode *base, const char *path);
int     vfs_rmdir_at(struct vnode *base, const char *path);
int     vfs_rename_at(struct vnode *obase, const char *from, struct vnode *nbase, const char *to);
int     vfs_symlink_at(const char *target, struct vnode *base, const char *path);
int     vfs_link_at(struct vnode *obase, const char *from, bool follow, struct vnode *nbase, const char *to);
int     vfs_readlink_at(struct vnode *base, const char *path, char *buf, size_t size);
int     vfs_stat_at(struct vnode *base, const char *path, bool follow, struct stat *st);
int     vfs_chmod_at(struct vnode *base, const char *path, bool follow, mode_t mode);
int     vfs_chown_at(struct vnode *base, const char *path, bool follow, int uid, int gid);
int     vfs_utime_at(struct vnode *base, const char *path, bool follow, int64_t mtime);
/* the same on an open file's node (fchmod, fchown, futimens) */
int     vfs_chmod_vn(struct vnode *vn, mode_t mode);
int     vfs_chown_vn(struct vnode *vn, int uid, int gid);
int     vfs_utime_vn(struct vnode *vn, int64_t mtime);
int     vfs_statfs(struct vnode *vn, struct kstatfs *st);
void    vfs_sync_all(bool final);               /* every mounted disk file system */
/* make the file system `m` the root ("/"), moving the other mounts into it */
int     vfs_pivot_root(struct mount *m);
int     vfs_mkdir(const char *path, mode_t mode);
int     vfs_mksock(const char *path, mode_t mode, struct vnode **out);   /* bind(2) */
int     vfs_unlink(const char *path);
int     vfs_rmdir(const char *path);
int     vfs_rename(const char *from, const char *to);
int     vfs_chmod(const char *path, mode_t mode);
int     vfs_chown(const char *path, int uid, int gid);
int     vfs_utime(const char *path, int64_t mtime);     /* mtime < 0: now */
int     vfs_stat(const char *path, struct stat *st);
int     vfs_chdir(const char *path);
int     vfs_getcwd(char *buf, size_t size);
int     vfs_path_of(struct vnode *vn, char *buf, size_t size);

/* ---- open files ---------------------------------------------------------- */
int     vfs_open(const char *path, int flags, mode_t mode, struct file **out);
ssize_t vfs_read(struct file *f, void *buf, size_t len);
ssize_t vfs_write(struct file *f, const void *buf, size_t len);
ssize_t vfs_pread(struct file *f, void *buf, size_t len, uint64_t off);     /* position untouched */
ssize_t vfs_pwrite(struct file *f, const void *buf, size_t len, uint64_t off);
off_t   vfs_lseek(struct file *f, off_t off, int whence);
int     vfs_getdents(struct file *f, void *buf, size_t len);
void    vfs_fill_stat(struct vnode *vn, struct stat *st);
int     vfs_ioctl(struct file *f, unsigned long req, void *arg);
int     vfs_fstat(struct file *f, struct stat *st);
int     vfs_poll(struct file *f, int events);   /* ready subset of events (+ERR/HUP) */
uint64_t vfs_poll_deadline(struct file *f);     /* see vnode_ops.poll_deadline */
bool     file_nolock(struct file *f);           /* see vnode_ops.nolock; false for NULL */
struct file *vfs_file_new(struct vnode *vn, int flags);    /* refcnt 1 */
int     pipe_create(struct file **rd, struct file **wr, int flags);   /* fs/pipe.c */
void    vfs_close(struct file *f);                  /* drops one reference */
int     vfs_flock(struct file *f, int op);          /* flock(2) LOCK_SH/EX/UN, LOCK_NB */
/* fcntl byte-range locks (fs/lockf.c); kfl is a kernel copy of struct flock */
int     vfs_lock_get(struct file *f, void *kfl, bool ofd);
int     vfs_lock_set(struct file *f, void *kfl, bool ofd, bool wait);
void    vfs_locks_fd_closed(struct file *f);        /* the process closes a descriptor of f */
void    vfs_locks_file_gone(struct file *f);        /* the open file's last reference */
struct file *vfs_file_get(struct file *f);         /* takes one (atomic), returns f */
int     vfs_truncate(struct vnode *vn, uint64_t size);
struct vm_object *vfs_vmobject(struct vnode *vn);  /* see vnode_ops.vmobject */

/* ---- filesystem drivers ------------------------------------------------- */
extern const struct vnode_ops ramfs_ops;
void devfs_init(const char *mountpoint);
/* Add a character/block-style device node to /dev after boot (drivers). */
/* fs/pty.c: /dev/ptmx and /dev/pts/N (created by devfs_init) */
void          pty_init(struct vnode *devroot);
int           pty_open_ctty(int flags, struct file **out);    /* /dev/tty of a pty session */
/* Console ptys (kernel/vt.c): the kernel holds the master; `tty_node`
 * (/dev/ttyN) becomes the slave. Returns the pty index or -errno. */
int           pty_console_create(struct vnode *tty_node, int rows, int cols);
ssize_t       pty_console_read(int idx, void *buf, size_t len);         /* output, blocks */
void          pty_console_input(int idx, const void *buf, size_t len);  /* typed bytes */
void          pty_console_hangup(int idx);      /* session over: SIGHUP, fresh termios */

struct vnode *devfs_register(const char *name, uint32_t mode, uint32_t rdev,
                             const struct vnode_ops *ops, void *ctx);
void procfs_init(const char *mountpoint);
void sysfs_init(const char *mountpoint);

/* Pseudo-file helpers used by procfs/sysfs: content is produced on demand
 * by a generator writing into a buffer of `cap` bytes. */
typedef size_t (*vfs_gen_t)(char *buf, size_t cap, void *ctx);
struct vnode *pseudo_dir(struct vnode *parent, const char *name);
struct vnode *pseudo_file(struct vnode *parent, const char *name, vfs_gen_t gen, void *ctx);
struct vnode *ramfs_write_file(const char *path, const char *text);
struct file  *ramfs_anon_file(const char *name, int flags);    /* memfd_create */

/* fs/epoll.c */
int      epoll_create_file(int flags, struct file **out);
int      epoll_ctl_file(struct file *ep, int op, int fd, struct file *target, uint32_t events, uint64_t data);
int      epoll_wait_file(struct file *ep, void *kevents, int maxevents, int64_t timeout_ms);
bool     epoll_is(struct file *f);
void     epoll_file_closing(struct file *f);    /* last close of a watched file */
void     epoll_file_used(struct file *f);       /* I/O on it: re-arm edge-triggered watches */

/* fs/anonfd.c */
int      eventfd_create(uint32_t initval, int flags, struct file **out);
int      timerfd_create_file(int clockid, int flags, struct file **out);
int      timerfd_settime_file(struct file *f, int flags, const int64_t nv[4], int64_t old[4]);
int      timerfd_gettime_file(struct file *f, int64_t cur[4]);
int      signalfd_file(struct file *f, uint64_t mask, int flags, struct file **out);

#endif
