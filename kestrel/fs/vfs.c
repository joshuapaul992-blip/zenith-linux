/* fs/vfs.c -- VFS core: node tree, mounts, path resolution, open files,
 * plus the two generic node flavours used by every filesystem:
 *   ramfs   - regular files backed by a growable heap buffer
 *   pseudo  - read-only files whose content is generated on each read */
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/vmobj.h>
#include <kernel/cpu.h>
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
    return (t && t->fs && t->fs->cwd) ? t->fs->cwd : root;
}

static struct vnode *child_named(struct vnode *dir, const char *name)
{
    if (dir->ops && dir->ops->refresh) dir->ops->refresh(dir);
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

static void node_destroy(struct vnode *vn)
{
    if (vn->ops == &ramfs_ops && vn->type == VREG && vn->data) vmobj_put(vn->data);
    kfree(vn);
}

/* A node leaves the tree. Like Linux, a file that is still open stays alive
 * (reads, writes and mappings keep working) until its last close. */
static void node_free(struct vnode *vn)
{
    uint64_t f = irq_save();
    vn->unlinked = true;
    bool now = __atomic_load_n(&vn->refs, __ATOMIC_ACQUIRE) == 0;
    irq_restore(f);
    if (now) node_destroy(vn);
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
    if (t && t->fs && t->fs->cwd == vn) t->fs->cwd = parent;    /* don't leave a dangling cwd */
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
    if (current_task()->fs) current_task()->fs->cwd = vn;
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
        vn = vfs_node_new(parent->fs, name, VREG, mode & ~(t && t->fs ? t->fs->umask : 022) & 0777, &ramfs_ops);
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

    if ((flags & O_TRUNC) && vn->type == VREG && acc != O_RDONLY && vn->ops && vn->ops->truncate)
        vn->ops->truncate(vn, 0);
    if (vn->ops && vn->ops->open) {
        int r2 = vn->ops->open(vn, flags, out);
        if (r2 == 0 && (*out)->vn) __atomic_add_fetch(&(*out)->vn->refs, 1, __ATOMIC_ACQ_REL);
        return r2;
    }

    struct file *f = kzalloc(sizeof *f);
    if (!f) return -ENOMEM;
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    __atomic_add_fetch(&vn->refs, 1, __ATOMIC_ACQ_REL);
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

/* Read at an offset without touching the file position (page faults of
 * file mappings, the ELF loader, pread64). Regular files only. */
ssize_t vfs_pread(struct file *f, void *buf, size_t len, uint64_t off)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->vn->type == VDIR) return -EISDIR;
    if (f->vn->type != VREG || !f->vn->ops || !f->vn->ops->read) return -ESPIPE;
    return f->vn->ops->read(f->vn, buf, len, off);
}

ssize_t vfs_pwrite(struct file *f, const void *buf, size_t len, uint64_t off)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->vn->type != VREG || !f->vn->ops || !f->vn->ops->write) return -ESPIPE;
    ssize_t n = f->vn->ops->write(f->vn, buf, len, off);
    if (n > 0) f->vn->mtime = (uint64_t)time_realtime_sec();
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
    if (f->off == 0 && dir->ops && dir->ops->refresh) dir->ops->refresh(dir);
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
    if (f->vn->ops && f->vn->ops->fioctl) return f->vn->ops->fioctl(f, req, arg);
    if (!f->vn->ops || !f->vn->ops->ioctl) return -ENOTTY;
    return f->vn->ops->ioctl(f->vn, req, arg);
}

int vfs_fstat(struct file *f, struct stat *st) { fill_stat(f->vn, st); return 0; }

/* Reference counts change with locked instructions: descriptors are shared
 * between threads, duplicated by dup/fork and carried in SCM_RIGHTS
 * messages, and the last vfs_close() alone releases the object. */
struct file *vfs_file_get(struct file *f)
{
    if (f) __atomic_add_fetch(&f->refcnt, 1, __ATOMIC_ACQ_REL);
    return f;
}

void vfs_close(struct file *f)
{
    if (!f || __atomic_sub_fetch(&f->refcnt, 1, __ATOMIC_ACQ_REL) > 0) return;
    struct vnode *vn = f->vn;
    bool last = vn && __atomic_sub_fetch(&vn->refs, 1, __ATOMIC_ACQ_REL) == 0 && vn->unlinked;
    if (vn && vn->ops && vn->ops->release) vn->ops->release(f);    /* may free a socket's vnode */
    kfree(f);
    if (last) node_destroy(vn);
}

struct file *vfs_file_new(struct vnode *vn, int flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    if (vn) __atomic_add_fetch(&vn->refs, 1, __ATOMIC_ACQ_REL);
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

/* Regular files keep their bytes in a vm_object (created on first write),
 * so shared mappings of the file and read()/write() stay coherent. */
static ssize_t ramfs_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    return vn->data ? vmobj_read(vn->data, buf, len, off) : 0;
}

static struct vm_object *ramfs_vmobject(struct vnode *vn)
{
    if (vn->type != VREG) return NULL;
    if (!vn->data) vn->data = vmobj_new(0);
    return vn->data;
}

static ssize_t ramfs_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    struct vm_object *o = ramfs_vmobject(vn);
    if (!o) return -ENOSPC;
    if (o->seals & 0x0008 && len) return -EPERM;        /* F_SEAL_WRITE */
    if ((o->seals & 0x0004) && off + len > o->size) return -EPERM;    /* F_SEAL_GROW */
    ssize_t n = vmobj_write(o, buf, len, off);
    if (n > 0) vn->size = o->size;
    return n;
}

static uint64_t ramfs_size(struct vnode *vn)
{
    return vn->type == VREG && vn->data ? ((struct vm_object *)vn->data)->size : 0;
}

static int ramfs_truncate(struct vnode *vn, uint64_t size)
{
    struct vm_object *o = ramfs_vmobject(vn);
    if (!o) return -ENOMEM;
    if ((o->seals & 0x0002) && size < o->size) return -EPERM;   /* F_SEAL_SHRINK */
    if ((o->seals & 0x0004) && size > o->size) return -EPERM;   /* F_SEAL_GROW */
    int r = vmobj_truncate(o, size);
    if (!r) vn->size = size;
    return r;
}

const struct vnode_ops ramfs_ops = { .read = ramfs_read, .write = ramfs_write, .size = ramfs_size,
                                     .truncate = ramfs_truncate, .vmobject = ramfs_vmobject };

/* An open file on a ramfs node that is in no directory (memfd_create):
 * freed with its last close. */
struct file *ramfs_anon_file(const char *name, int flags)
{
    struct vnode *vn = vfs_node_new(root ? root->fs : NULL, name, VREG, 0600, &ramfs_ops);
    if (!vn) return NULL;
    vn->unlinked = true;
    if (!ramfs_vmobject(vn)) { kfree(vn); return NULL; }
    struct file *f = vfs_file_new(vn, flags);
    if (!f) { node_destroy(vn); return NULL; }
    return f;
}

int vfs_truncate(struct vnode *vn, uint64_t size)
{
    if (vn->type == VDIR) return -EISDIR;
    if (vn->type != VREG) return -EINVAL;
    if (vn->fs && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    if (!vn->ops || !vn->ops->truncate) return -EINVAL;
    int r = vn->ops->truncate(vn, size);
    if (!r) vn->mtime = (uint64_t)time_realtime_sec();
    return r;
}

struct vm_object *vfs_vmobject(struct vnode *vn)
{
    return vn && vn->ops && vn->ops->vmobject ? vn->ops->vmobject(vn) : NULL;
}

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

#define PSEUDO_BUF 65536            /* /proc/PID/maps of a large program */

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

/* Make directory `dst` show the contents of directory `src` (a bind mount,
 * e.g. /boot/bin on /bin). The source keeps its place in the tree, so ".."
 * inside the bound directory leads to the source's parent. */
int vfs_bind(const char *src, const char *dst)
{
    struct vnode *s, *d;
    int r;
    if ((r = vfs_lookup(src, &s)) < 0 || (r = vfs_lookup(dst, &d)) < 0) return r;
    if (s->type != VDIR || d->type != VDIR) return -ENOTDIR;
    if (d->mounted || s == d) return -EBUSY;
    struct mount *m = kzalloc(sizeof *m);
    if (!m) return -ENOMEM;
    strlcpy(m->fstype, "bind", sizeof m->fstype);
    m->flags = s->fs ? s->fs->flags : 0;
    m->dev = s->fs ? s->fs->dev : 0;
    m->root = s;
    m->covered = d;
    strlcpy(m->path, dst, sizeof m->path);
    d->mounted = s;
    struct mount **pp = &mounts;
    while (*pp) pp = &(*pp)->next;
    *pp = m;
    kprintf("vfs: bound %s on %s\n", src, dst);
    return 0;
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
        "/bin", "/dev", "/etc", "/home", "/lib", "/mnt", "/proc", "/root",
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
        "root:x:0:0:root:/root:/bin/sh\n"
        "user:x:1000:1000:Kestrel User:/home/user:/bin/sh\n");
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
    ramfs_write_file("/etc/profile",
        "# /etc/profile -- read by the login shells on the text terminals (kernel/vt.c)\n"
        "PS1='\\[\\e[92m\\]\\u@\\h\\[\\e[0m\\]:\\[\\e[94m\\]\\w\\[\\e[0m\\]\\$ '\n"
        "export PS1\n"
        "cd \"$HOME\" 2>/dev/null\n"
        "echo \"$(uname -sr) on $(tty). Ctrl+Alt+F1..F12: other monitors; startx: the desktop.\"\n");
    ramfs_write_file("/etc/motd",
        "Welcome to Kestrel -- a freestanding x86_64 POSIX-style kernel.\n"
        "Everything above was read through open/read/getdents64 system calls.\n");
}
