/* fs/vfs.c -- VFS core: node tree, mounts, path resolution, open files,
 * plus the two generic node flavours used by every filesystem:
 *   ramfs   - regular files backed by a growable heap buffer
 *   pseudo  - read-only files whose content is generated on each read */
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/arch.h>
#include <kernel/time.h>
#include <kernel/bootinfo.h>

static struct vnode *root;
static struct mount *mounts;
static uint64_t next_ino = 1;
static uint32_t next_dev = 1;

struct vnode *vfs_root(void) { return root; }
struct mount *vfs_mounts(void) { return mounts; }

/* ======================================================================= */
/*  node management                                                          */
/* ======================================================================= */

struct vnode *vfs_node_new(struct mount *fs, const char *name, enum vtype t, uint32_t mode,
                           const struct vnode_ops *ops)
{
    struct vnode *vn = kzalloc(sizeof *vn);
    if (!vn) return NULL;
    strlcpy(vn->name, name, sizeof vn->name);
    vn->type  = t;
    vn->mode  = mode | (t == VDIR ? S_IFDIR : t == VCHR ? S_IFCHR : t == VSOCK ? S_IFSOCK : S_IFREG);
    vn->ino   = next_ino++;
    vn->ops   = ops;
    vn->fs    = fs;
    vn->mtime = (uint64_t)time_realtime_sec();
    return vn;
}

void vfs_node_add(struct vnode *dir, struct vnode *child)
{
    child->parent = dir;
    child->sibling = NULL;
    struct vnode **pp = &dir->children;         /* append: keeps creation order */
    while (*pp) pp = &(*pp)->sibling;
    *pp = child;
}

static void node_remove(struct vnode *dir, struct vnode *child)
{
    for (struct vnode **pp = &dir->children; *pp; pp = &(*pp)->sibling)
        if (*pp == child) { *pp = child->sibling; return; }
}

struct vnode *vfs_mkfs(const char *fstype, uint32_t flags, struct mount **out)
{
    struct mount *m = kzalloc(sizeof *m);
    strlcpy(m->fstype, fstype, sizeof m->fstype);
    m->flags = flags;
    m->dev = next_dev++;
    m->root = vfs_node_new(m, "", VDIR, 0755, &ramfs_ops);
    *out = m;
    return m->root;
}

int vfs_mount(struct mount *m, const char *path)
{
    if (!root) {                                    /* the root filesystem */
        root = m->root;
        root->parent = root;
        strlcpy(m->path, "/", sizeof m->path);
    } else {
        struct vnode *dir;
        int r = vfs_lookup(path, &dir);
        if (r < 0) return r;
        if (dir->type != VDIR) return -ENOTDIR;
        if (dir->mounted) return -EBUSY;
        dir->mounted = m->root;
        m->covered = dir;
        m->root->parent = dir->parent;
        strlcpy(m->root->name, dir->name, sizeof m->root->name);
        strlcpy(m->path, path, sizeof m->path);
    }
    struct mount **pp = &mounts;
    while (*pp) pp = &(*pp)->next;
    *pp = m;
    kprintf("vfs: mounted %-6s on %s%s\n", m->fstype, m->path, (m->flags & MNT_RDONLY) ? " (ro)" : "");
    return 0;
}

/* ======================================================================= */
/*  path resolution                                                          */
/* ======================================================================= */

static struct vnode *cwd_node(void)
{
    struct tcb *t = current_task();
    return (t && t->cwd) ? t->cwd : root;
}

static struct vnode *child_named(struct vnode *dir, const char *name)
{
    for (struct vnode *c = dir->children; c; c = c->sibling)
        if (strcmp(c->name, name) == 0) return c->mounted ? c->mounted : c;
    return NULL;
}

/* Resolve `path`. If `parent_out` is non-NULL, resolve everything except
 * the final component, return the parent directory and copy the final
 * component into `last` (VFS_NAME_MAX bytes). */
static int walk(const char *path, struct vnode **out, struct vnode **parent_out, char *last)
{
    if (!path || !*path) return -ENOENT;
    if (strlen(path) >= VFS_PATH_MAX) return -ENAMETOOLONG;

    char buf[VFS_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    struct vnode *vn = path[0] == '/' ? root : cwd_node();

    /* split into components first so we know which one is last */
    char *comps[64]; int n = 0; char *save;
    for (char *tok = strtok_r(buf, "/", &save); tok && n < 64; tok = strtok_r(NULL, "/", &save))
        comps[n++] = tok;

    int stop = parent_out ? n - 1 : n;
    for (int i = 0; i < stop; i++) {
        if (vn->type != VDIR) return -ENOTDIR;
        if (strcmp(comps[i], ".") == 0) continue;
        if (strcmp(comps[i], "..") == 0) { vn = vn->parent ? vn->parent : root; continue; }
        if (strlen(comps[i]) >= VFS_NAME_MAX) return -ENAMETOOLONG;
        struct vnode *c = child_named(vn, comps[i]);
        if (!c) return -ENOENT;
        vn = c;
    }
    if (parent_out) {
        if (n == 0) return -EEXIST;                 /* path was "/" */
        if (vn->type != VDIR) return -ENOTDIR;
        if (strlen(comps[n - 1]) >= VFS_NAME_MAX) return -ENAMETOOLONG;
        *parent_out = vn;
        strlcpy(last, comps[n - 1], VFS_NAME_MAX);
        return 0;
    }
    *out = vn;
    return 0;
}

int vfs_lookup(const char *path, struct vnode **out) { return walk(path, out, NULL, NULL); }

int vfs_path_of(struct vnode *vn, char *buf, size_t size)
{
    const char *parts[64]; int n = 0;
    while (vn && vn != root && n < 64) { parts[n++] = vn->name; vn = vn->parent; }
    size_t pos = 0;
    if (n == 0) { if (size < 2) return -ERANGE; strlcpy(buf, "/", size); return 0; }
    buf[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (pos + 1 + strlen(parts[i]) + 1 > size) return -ERANGE;
        strlcat(buf, "/", size);
        strlcat(buf, parts[i], size);
        pos = strlen(buf);
    }
    return 0;
}

static int check_writable_dir(struct vnode *dir)
{
    if (dir->fs->flags & MNT_RDONLY) return -EROFS;
    if (dir->fs->flags & MNT_NOCREATE) return -EPERM;
    return 0;
}

int vfs_mkdir(const char *path, mode_t mode)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(path, NULL, &parent, name);
    if (r < 0) return r;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EEXIST;
    if (child_named(parent, name)) return -EEXIST;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct vnode *d = vfs_node_new(parent->fs, name, VDIR, mode & 0777, &ramfs_ops);
    if (!d) return -ENOMEM;
    vfs_node_add(parent, d);
    return 0;
}

/* A socket's name in the file system: a node with no operations that
 * connect(2) finds by path (fs/unixsock.c keeps the socket itself). */
int vfs_mksock(const char *path, mode_t mode, struct vnode **out)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(path, NULL, &parent, name);
    if (r < 0) return r;
    if (child_named(parent, name)) return -EADDRINUSE;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct vnode *s = vfs_node_new(parent->fs, name, VSOCK, mode & 0777, NULL);
    if (!s) return -ENOMEM;
    vfs_node_add(parent, s);
    *out = s;
    return 0;
}

/* Find the entry `name` in `dir` (the covered node, not a mount root). */
static struct vnode *entry_named(struct vnode *dir, const char *name)
{
    for (struct vnode *c = dir->children; c; c = c->sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static void node_free(struct vnode *vn)
{
    if (vn->ops == &ramfs_ops) kfree(vn->data);
    kfree(vn);
}

/* unlink(2): files and devices only */
int vfs_unlink(const char *path)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(path, NULL, &parent, name);
    if (r < 0) return r;
    struct vnode *vn = entry_named(parent, name);
    if (!vn) return -ENOENT;
    if (vn->type == VDIR) return -EISDIR;
    if ((r = check_writable_dir(parent)) < 0) return r;
    node_remove(parent, vn);
    node_free(vn);
    return 0;
}

/* rmdir(2): empty directories only, never a mount point or "." */
int vfs_rmdir(const char *path)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(path, NULL, &parent, name);
    if (r < 0) return r;
    if (!strcmp(name, ".")) return -EINVAL;
    if (!strcmp(name, "..")) return -ENOTEMPTY;
    struct vnode *vn = entry_named(parent, name);
    if (!vn) return -ENOENT;
    if (vn->type != VDIR) return -ENOTDIR;
    if (vn->mounted) return -EBUSY;
    if (vn->children) return -ENOTEMPTY;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct tcb *t = current_task();
    if (t && t->cwd == vn) t->cwd = parent;             /* don't leave a dangling cwd */
    node_remove(parent, vn);
    node_free(vn);
    return 0;
}

/* rename(2) within one mounted file system; -EXDEV across mounts. */
int vfs_rename(const char *from, const char *to)
{
    struct vnode *op, *np; char oname[VFS_NAME_MAX], nname[VFS_NAME_MAX];
    int r = walk(from, NULL, &op, oname);
    if (r < 0) return r;
    if ((r = walk(to, NULL, &np, nname)) < 0) return r;
    if (!strcmp(oname, ".") || !strcmp(oname, "..") || !strcmp(nname, ".") || !strcmp(nname, "..")) return -EINVAL;
    struct vnode *vn = entry_named(op, oname);
    if (!vn) return -ENOENT;
    if (vn->mounted) return -EBUSY;
    if (op->fs != np->fs) return -EXDEV;
    if ((r = check_writable_dir(op)) < 0 || (r = check_writable_dir(np)) < 0) return r;
    for (struct vnode *a = np; a; a = (a->parent == a ? NULL : a->parent)) {
        if (a == vn) return -EINVAL;                    /* into its own subtree */
        if (a == root) break;
    }
    struct vnode *old = entry_named(np, nname);
    if (old == vn) return 0;
    if (old) {
        if (old->mounted) return -EBUSY;
        if (old->type == VDIR && vn->type != VDIR) return -EISDIR;
        if (old->type != VDIR && vn->type == VDIR) return -ENOTDIR;
        if (old->type == VDIR && old->children) return -ENOTEMPTY;
        node_remove(np, old);
        node_free(old);
    }
    node_remove(op, vn);
    strlcpy(vn->name, nname, sizeof vn->name);
    vfs_node_add(np, vn);
    return 0;
}

static int attr_target(const char *path, struct vnode **out)
{
    int r = vfs_lookup(path, out);
    if (r < 0) return r;
    if ((*out)->fs && ((*out)->fs->flags & MNT_RDONLY)) return -EROFS;
    return 0;
}

int vfs_chmod(const char *path, mode_t mode)
{
    struct vnode *vn;
    int r = attr_target(path, &vn);
    if (r < 0) return r;
    vn->mode = (vn->mode & ~07777u) | (mode & 07777u);
    return 0;
}

int vfs_chown(const char *path, int uid, int gid)
{
    struct vnode *vn;
    int r = attr_target(path, &vn);
    if (r < 0) return r;
    if (uid != -1) vn->uid = (uint32_t)uid;
    if (gid != -1) vn->gid = (uint32_t)gid;
    return 0;
}

int vfs_utime(const char *path, int64_t mtime)
{
    struct vnode *vn;
    int r = attr_target(path, &vn);
    if (r < 0) return r;
    vn->mtime = (uint64_t)(mtime < 0 ? time_realtime_sec() : mtime);
    return 0;
}

static uint64_t node_size(struct vnode *vn)
{
    if (vn->type == VDIR) return 0;
    return vn->ops && vn->ops->size ? vn->ops->size(vn) : vn->size;
}

static void fill_stat(struct vnode *vn, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = vn->fs ? vn->fs->dev : 0;
    st->st_ino = vn->ino;
    st->st_nlink = vn->type == VDIR ? 2 : 1;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_rdev = vn->rdev;
    st->st_size = (int64_t)node_size(vn);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_mtim.tv_sec = st->st_ctim.tv_sec = st->st_atim.tv_sec = (int64_t)vn->mtime;
}

int vfs_stat(const char *path, struct stat *st)
{
    struct vnode *vn;
    int r = vfs_lookup(path, &vn);
    if (r < 0) return r;
    fill_stat(vn, st);
    return 0;
}

int vfs_chdir(const char *path)
{
    struct vnode *vn;
    int r = vfs_lookup(path, &vn);
    if (r < 0) return r;
    if (vn->type != VDIR) return -ENOTDIR;
    current_task()->cwd = vn;
    return 0;
}

int vfs_getcwd(char *buf, size_t size) { return vfs_path_of(cwd_node(), buf, size); }

/* ======================================================================= */
/*  open file objects                                                        */
/* ======================================================================= */

int vfs_open(const char *path, int flags, mode_t mode, struct file **out)
{
    struct vnode *vn;
    int r = vfs_lookup(path, &vn);
    bool created = false;

    if (r == -ENOENT && (flags & O_CREAT)) {
        created = true;
        struct vnode *parent; char name[VFS_NAME_MAX];
        if ((r = walk(path, NULL, &parent, name)) < 0) return r;
        if ((r = check_writable_dir(parent)) < 0) return r;
        struct tcb *t = current_task();
        vn = vfs_node_new(parent->fs, name, VREG, mode & ~(t ? t->umask : 022) & 0777, &ramfs_ops);
        if (!vn) return -ENOMEM;
        vfs_node_add(parent, vn);
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }

    int acc = flags & O_ACCMODE;
    if (vn->type == VSOCK) return -ENXIO;                   /* sockets are connect()ed, not opened */
    if (vn->type == VDIR && acc != O_RDONLY) return -EISDIR;
    if ((flags & O_DIRECTORY) && vn->type != VDIR) return -ENOTDIR;
    if (acc != O_RDONLY && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    /* POSIX: the creating open gets the access it asked for whatever the new
     * mode says; root (uid 0) is not limited by permission bits. */
    struct tcb *self = current_task();
    if (acc != O_RDONLY && !(vn->mode & 0222) && !created && self && self->uid != 0) return -EACCES;

    if ((flags & O_TRUNC) && vn->type == VREG && acc != O_RDONLY && vn->ops == &ramfs_ops)
        vn->size = 0;

    struct file *f = kzalloc(sizeof *f);
    if (!f) return -ENOMEM;
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    *out = f;
    return 0;
}

ssize_t vfs_read(struct file *f, void *buf, size_t len)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->vn->type == VDIR) return -EISDIR;
    if (f->vn->ops && f->vn->ops->fread) return f->vn->ops->fread(f, buf, len);
    if (!f->vn->ops || !f->vn->ops->read) return -EINVAL;
    ssize_t n = f->vn->ops->read(f->vn, buf, len, f->off);
    if (n > 0) f->off += (uint64_t)n;
    return n;
}

ssize_t vfs_write(struct file *f, const void *buf, size_t len)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->vn->ops && f->vn->ops->fwrite) return f->vn->ops->fwrite(f, buf, len);
    if (!f->vn->ops || !f->vn->ops->write) return -EINVAL;
    if (f->flags & O_APPEND) f->off = node_size(f->vn);
    ssize_t n = f->vn->ops->write(f->vn, buf, len, f->off);
    if (n > 0) { f->off += (uint64_t)n; f->vn->mtime = (uint64_t)time_realtime_sec(); }
    return n;
}

off_t vfs_lseek(struct file *f, off_t off, int whence)
{
    if (f->vn->type == VSOCK || (f->vn->type == VCHR && !(f->vn->ops && f->vn->ops->size))) return -ESPIPE;   /* ttys, null... */
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off
                 : whence == SEEK_END ? (int64_t)node_size(f->vn) : -1;
    if (base < 0 || base + off < 0) return -EINVAL;
    f->off = (uint64_t)(base + off);
    return (off_t)f->off;
}

static uint8_t dtype(struct vnode *vn)
{
    return vn->type == VDIR ? DT_DIR : vn->type == VCHR ? DT_CHR : vn->type == VREG ? DT_REG : DT_UNKNOWN;
}

/* getdents64: f->off is the index of the next entry ("." = 0, ".." = 1) */
int vfs_getdents(struct file *f, void *buf, size_t len)
{
    struct vnode *dir = f->vn;
    if (dir->type != VDIR) return -ENOTDIR;
    size_t pos = 0;
    for (;;) {
        uint64_t idx = f->off;
        const char *name; struct vnode *vn;
        if (idx == 0)      { name = ".";  vn = dir; }
        else if (idx == 1) { name = ".."; vn = dir->parent ? dir->parent : dir; }
        else {
            vn = dir->children;
            for (uint64_t i = 2; vn && i < idx; i++) vn = vn->sibling;
            if (!vn) break;
            name = vn->name;
        }
        size_t nlen = strlen(name);
        size_t reclen = (offsetof(struct linux_dirent64, d_name) + nlen + 1 + 7) & ~(size_t)7;
        if (pos + reclen > len) { if (pos == 0) return -EINVAL; break; }
        struct linux_dirent64 *d = (struct linux_dirent64 *)((uint8_t *)buf + pos);
        struct vnode *shown = vn->mounted ? vn->mounted : vn;
        d->d_ino = shown->ino;
        d->d_off = (int64_t)idx + 1;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = dtype(shown);
        memcpy(d->d_name, name, nlen + 1);
        pos += reclen;
        f->off++;
    }
    return (int)pos;
}

int vfs_ioctl(struct file *f, unsigned long req, void *arg)
{
    if (!f->vn->ops || !f->vn->ops->ioctl) return -ENOTTY;
    return f->vn->ops->ioctl(f->vn, req, arg);
}

int vfs_fstat(struct file *f, struct stat *st) { fill_stat(f->vn, st); return 0; }

void vfs_close(struct file *f)
{
    if (!f || --f->refcnt > 0) return;
    if (f->vn && f->vn->ops && f->vn->ops->release) f->vn->ops->release(f);
    kfree(f);
}

struct file *vfs_file_new(struct vnode *vn, int flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    return f;
}

/* Files without a poll op (regular files, most devices) never block. */
int vfs_poll(struct file *f, int events)
{
    if (f->vn && f->vn->ops && f->vn->ops->poll) return f->vn->ops->poll(f, events);
    int r = 0;
    if ((f->flags & O_ACCMODE) != O_WRONLY) r |= POLLIN;
    if ((f->flags & O_ACCMODE) != O_RDONLY) r |= POLLOUT;
    return r & events;
}

/* ======================================================================= */
/*  ramfs: heap-backed regular files                                          */
/* ======================================================================= */

static ssize_t ramfs_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    if (off >= vn->size) return 0;
    if (len > vn->size - off) len = vn->size - off;
    memcpy(buf, (uint8_t *)vn->data + off, len);
    return (ssize_t)len;
}

static ssize_t ramfs_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    size_t need = off + len;
    if (need > vn->cap) {
        size_t cap = vn->cap ? vn->cap : 256;
        while (cap < need) cap *= 2;
        void *nd = krealloc(vn->data, cap);
        if (!nd) return -ENOSPC;
        vn->data = nd; vn->cap = cap;
    }
    if (off > vn->size) memset((uint8_t *)vn->data + vn->size, 0, off - vn->size);
    memcpy((uint8_t *)vn->data + off, buf, len);
    if (need > vn->size) vn->size = need;
    return (ssize_t)len;
}

const struct vnode_ops ramfs_ops = { .read = ramfs_read, .write = ramfs_write };

struct vnode *ramfs_write_file(const char *path, const char *text)
{
    struct file *f;
    if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644, &f) < 0) return NULL;
    vfs_write(f, text, strlen(text));
    struct vnode *vn = f->vn;
    vfs_close(f);
    return vn;
}

/* ======================================================================= */
/*  pseudo files: generated content (procfs, sysfs)                           */
/* ======================================================================= */

#define PSEUDO_BUF 8192

static ssize_t pseudo_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    char *tmp = kmalloc(PSEUDO_BUF);
    if (!tmp) return -ENOMEM;
    size_t n = ((vfs_gen_t)vn->data)(tmp, PSEUDO_BUF, vn->ctx);
    if (n > PSEUDO_BUF) n = PSEUDO_BUF;
    ssize_t r = 0;
    if (off < n) {
        r = (ssize_t)(len < n - off ? len : n - off);
        memcpy(buf, tmp + off, (size_t)r);
    }
    kfree(tmp);
    return r;
}

static uint64_t pseudo_size(struct vnode *vn)
{
    char *tmp = kmalloc(PSEUDO_BUF);
    if (!tmp) return 0;
    size_t n = ((vfs_gen_t)vn->data)(tmp, PSEUDO_BUF, vn->ctx);
    kfree(tmp);
    return n > PSEUDO_BUF ? PSEUDO_BUF : n;
}

static const struct vnode_ops pseudo_ops = { .read = pseudo_read, .size = pseudo_size };

struct vnode *pseudo_dir(struct vnode *parent, const char *name)
{
    struct vnode *d = vfs_node_new(parent->fs, name, VDIR, 0555, NULL);   /* directories have no data ops */
    vfs_node_add(parent, d);
    return d;
}

struct vnode *pseudo_file(struct vnode *parent, const char *name, vfs_gen_t gen, void *ctx)
{
    struct vnode *f = vfs_node_new(parent->fs, name, VREG, 0444, &pseudo_ops);
    f->data = (void *)gen;
    f->ctx = ctx;
    vfs_node_add(parent, f);
    return f;
}

/* ======================================================================= */
/*  boot-time layout                                                          */
/* ======================================================================= */

void vfs_init(void)
{
    struct mount *rootfs;
    vfs_mkfs("ramfs", 0, &rootfs);
    vfs_mount(rootfs, "/");

    static const char *const dirs[] = {
        "/bin", "/dev", "/etc", "/home", "/mnt", "/proc", "/root",
        "/sys", "/tmp", "/usr", "/usr/bin", "/var", "/var/log",
    };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) vfs_mkdir(dirs[i], 0755);
    struct vnode *tmp;
    if (vfs_lookup("/tmp", &tmp) == 0) tmp->mode |= 01777;

    devfs_init("/dev");
    procfs_init("/proc");
    sysfs_init("/sys");

    ramfs_write_file("/etc/hostname", KESTREL_HOSTNAME "\n");
    ramfs_write_file("/etc/passwd",
        "root:x:0:0:root:/root:/bin/kush\n"
        "user:x:1000:1000:Kestrel User:/home/user:/bin/kush\n");
    ramfs_write_file("/etc/group", "root:x:0:\nwheel:x:10:root\nuser:x:1000:\n");
    vfs_mkdir("/home/user", 0755);
    vfs_chown("/home/user", 1000, 1000);
    ramfs_write_file("/etc/os-release",
        "NAME=\"Kestrel OS\"\nVERSION=\"" KESTREL_VERSION " (" KESTREL_CODENAME ")\"\nID=kestrel\n"
        "PRETTY_NAME=\"Kestrel OS " KESTREL_VERSION " x86_64\"\n");
    ramfs_write_file("/etc/fstab",
        "# <fs>   <mountpoint>  <type>   <options>\n"
        "rootfs   /             ramfs    rw\n"
        "devfs    /dev          devfs    rw,nocreate\n"
        "proc     /proc         procfs   ro\n"
        "sysfs    /sys          sysfs    ro\n");
    ramfs_write_file("/etc/motd",
        "Welcome to Kestrel -- a freestanding x86_64 POSIX-style kernel.\n"
        "Everything above was read through open/read/getdents64 system calls.\n");
}
