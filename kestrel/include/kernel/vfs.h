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

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256

enum vtype { VNON = 0, VREG, VDIR, VCHR, VSOCK };

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
    void    (*release)(struct file *f);            /* last close of f      */
    int     (*fioctl)(struct file *f, unsigned long req, void *arg);  /* preferred over ioctl */
    /* Optional: create the open file (devices that hand out a new object per
     * open, e.g. /dev/ptmx). Called after vfs_open's permission checks. */
    int     (*open)(struct vnode *vn, int flags, struct file **out);
    /* Optional, directories: bring the children up to date before a lookup
     * or listing (/proc's per-process directories). */
    void    (*refresh)(struct vnode *dir);
};

/* poll(2) bits */
#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

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
};

#define MNT_RDONLY   0x1
#define MNT_NOCREATE 0x2            /* pseudo fs: structure fixed by kernel */

struct mount {
    char          path[VFS_PATH_MAX];
    char          fstype[16];
    struct vnode *root;
    struct vnode *covered;
    uint32_t      flags;
    uint32_t      dev;
    struct mount *next;
};

struct file {
    struct vnode *vn;
    uint64_t      off;
    int           flags;
    int           refcnt;
};

/* ---- core ------------------------------------------------------------- */
void          vfs_init(void);
struct vnode *vfs_root(void);
struct mount *vfs_mounts(void);
struct vnode *vfs_node_new(struct mount *fs, const char *name, enum vtype t, uint32_t mode,
                           const struct vnode_ops *ops);
void          vfs_node_add(struct vnode *dir, struct vnode *child);
struct vnode *vfs_mkfs(const char *fstype, uint32_t flags, struct mount **out);
int           vfs_mount(struct mount *m, const char *path);
int           vfs_bind(const char *src, const char *dst);     /* show src's contents at dst */

/* ---- path operations (relative paths use the current task's cwd) ------- */
int     vfs_lookup(const char *path, struct vnode **out);
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
off_t   vfs_lseek(struct file *f, off_t off, int whence);
int     vfs_getdents(struct file *f, void *buf, size_t len);
int     vfs_ioctl(struct file *f, unsigned long req, void *arg);
int     vfs_fstat(struct file *f, struct stat *st);
int     vfs_poll(struct file *f, int events);   /* ready subset of events (+ERR/HUP) */
struct file *vfs_file_new(struct vnode *vn, int flags);    /* refcnt 1 */
int     pipe_create(struct file **rd, struct file **wr, int flags);   /* fs/pipe.c */
void    vfs_close(struct file *f);

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

#endif
