/* fs/vfs.c -- VFS core: node tree, mounts, path resolution, open files,
 * plus the two generic node flavours used by every filesystem:
 *   ramfs   - regular files backed by a growable heap buffer
 *   pseudo  - read-only files whose content is generated on each read */
#include <kernel/smp.h>
#include <kernel/vfs.h>
#include <kernel/inotify.h>
#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/vmobj.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/arch.h>
#include <kernel/time.h>
#include <kernel/bootinfo.h>
#include <kernel/process.h>

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
    if (dir->dnext < 2) dir->dnext = 2;                 /* 0 and 1 are "." and ".." */
    child->dpos = dir->dnext++;                         /* stable while others come and go */
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

/* The target of symbolic link vn into buf (NUL-terminated); its length. */
static int link_target(struct vnode *vn, char *buf, size_t size)
{
    if (vn->type != VLNK || !vn->ops || !vn->ops->readlink) return -EINVAL;
    int n = vn->ops->readlink(vn, buf, size - 1);
    if (n < 0) return n;
    buf[n] = 0;
    return n;
}

#define WALK_FOLLOW 1           /* follow a symbolic link in the last component */
#define WALK_PARENT 2           /* stop before the last component, return its name */

/* Resolve `path` from `base` (NULL: the cwd). WALK_PARENT: return the
 * directory holding the last component in *out and copy that component into
 * `last` (VFS_NAME_MAX bytes); the last component itself is not looked up.
 * Symbolic links in the middle of the path are always followed: the rest of
 * the path is appended to the link's target and the walk goes on from "/"
 * or from the link's directory. */
static int walk(struct vnode *base, const char *path, int flags, struct vnode **out, char *last)
{
    if (!path || !*path) return -ENOENT;
    size_t plen = strlen(path);
    if (plen >= VFS_PATH_MAX) return -ENAMETOOLONG;
    const size_t cap = 2 * VFS_PATH_MAX;
    char *buf = kmalloc(cap), *tmp = kmalloc(cap);
    int r = 0;
    if (!buf || !tmp) { r = -ENOMEM; goto out; }
    memcpy(buf, path, plen + 1);
    char *p = buf;
    struct vnode *vn = path[0] == '/' ? root : (base ? base : cwd_node());
    int links = 0;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) {                                      /* no component left */
            if (flags & WALK_PARENT) { r = -EEXIST; goto out; }    /* the path was "/" */
            *out = vn;
            goto out;
        }
        char *e = p;
        while (*e && *e != '/') e++;
        size_t clen = (size_t)(e - p);
        if (clen >= VFS_NAME_MAX) { r = -ENAMETOOLONG; goto out; }
        char comp[VFS_NAME_MAX];
        memcpy(comp, p, clen);
        comp[clen] = 0;
        bool is_last = true;
        for (char *q = e; *q; q++) if (*q != '/') { is_last = false; break; }
        bool slash = *e == '/';                         /* "name/": must be a directory */
        if (vn->type != VDIR) { r = -ENOTDIR; goto out; }
        if (is_last && (flags & WALK_PARENT)) {
            *out = vn;
            memcpy(last, comp, clen + 1);
            goto out;
        }
        if (!strcmp(comp, ".")) { p = e; continue; }
        if (!strcmp(comp, "..")) { vn = vn->parent ? vn->parent : root; p = e; continue; }
        struct vnode *c = child_named(vn, comp);
        if (!c) { r = -ENOENT; goto out; }
        if (c->type == VLNK && (!is_last || slash || (flags & WALK_FOLLOW))) {
            if (++links > VFS_MAX_SYMLINKS) { r = -ELOOP; goto out; }
            int tl = link_target(c, tmp, VFS_PATH_MAX);
            if (tl < 0) { r = tl; goto out; }
            if (tl == 0) { r = -ENOENT; goto out; }
            size_t rl = strlen(e);
            if ((size_t)tl + rl + 1 > cap) { r = -ENAMETOOLONG; goto out; }
            memcpy(tmp + tl, e, rl + 1);                /* target + the rest of the path */
            char *sw = buf; buf = tmp; tmp = sw;
            p = buf;
            if (*p == '/') vn = root;                   /* else: from the link's directory */
            continue;
        }
        vn = c;
        p = e;
    }
out:
    kfree(buf);
    kfree(tmp);
    return r;
}

int vfs_lookup_at(struct vnode *base, const char *path, bool follow, struct vnode **out)
{
    return walk(base, path, follow ? WALK_FOLLOW : 0, out, NULL);
}

int vfs_lookup(const char *path, struct vnode **out) { return walk(NULL, path, WALK_FOLLOW, out, NULL); }

int vfs_path_of(struct vnode *vn, char *buf, size_t size)
{
    const char *parts[128]; int n = 0;
    while (vn && vn != root && n < 128) { parts[n++] = vn->name; vn = vn->parent; }
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

static const struct vnode_ops ramfs_link_ops;

/* A new entry `name` in `dir`: through the file system's create operation
 * if it has one, else an in-memory node. Owned by the caller's uid/gid. */
static int create_node(struct vnode *dir, const char *name, enum vtype t, uint32_t mode, const char *target,
                       struct vnode **out)
{
    int r = check_writable_dir(dir);
    if (r < 0) return r;
    struct tcb *self = current_task();
    struct vnode *vn = NULL;
    if (dir->ops && dir->ops->create) {
        if ((r = dir->ops->create(dir, name, t, mode, target, &vn)) < 0) return r;
    } else {
        vn = vfs_node_new(dir->fs, name, t, mode & 07777, t == VLNK ? &ramfs_link_ops : &ramfs_ops);
        if (!vn) return -ENOMEM;
        if (t == VLNK) {
            vn->mode = S_IFLNK | 0777;
            if (!(vn->data = kstrdup(target))) { kfree(vn); return -ENOMEM; }
            vn->size = strlen(target);
        }
        if (self) { vn->uid = self->uid; vn->gid = self->gid; }
    }
    strlcpy(vn->name, name, sizeof vn->name);
    vfs_node_add(dir, vn);
    *out = vn;
    inotify_event(dir, IN_CREATE | (t == VDIR ? IN_ISDIR : 0), name, 0);
    return 0;
}

static int setattr(struct vnode *vn)
{
    vn->ctime = (uint64_t)time_realtime_sec();
    int r = vn->ops && vn->ops->setattr ? vn->ops->setattr(vn) : 0;
    if (!r) inotify_self(vn, IN_ATTRIB);
    return r;
}

int vfs_mkdir_at(struct vnode *base, const char *path, mode_t mode)
{
    struct vnode *parent, *d; char name[VFS_NAME_MAX];
    int r = walk(base, path, WALK_PARENT, &parent, name);
    if (r < 0) return r;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EEXIST;
    if (child_named(parent, name)) return -EEXIST;
    struct tcb *t = current_task();
    return create_node(parent, name, VDIR, mode & 07777 & ~(t && t->fs ? t->fs->umask : 022), NULL, &d);
}

int vfs_mkdir(const char *path, mode_t mode) { return vfs_mkdir_at(NULL, path, mode); }

int vfs_symlink_at(const char *target, struct vnode *base, const char *path)
{
    struct vnode *parent, *l; char name[VFS_NAME_MAX];
    if (!target || !*target) return -ENOENT;
    if (strlen(target) >= VFS_PATH_MAX) return -ENAMETOOLONG;
    int r = walk(base, path, WALK_PARENT, &parent, name);
    if (r < 0) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..") || child_named(parent, name)) return -EEXIST;
    return create_node(parent, name, VLNK, 0777, target, &l);
}

int vfs_readlink_at(struct vnode *base, const char *path, char *buf, size_t size)
{
    struct vnode *vn;
    int r = walk(base, path, 0, &vn, NULL);
    if (r < 0) return r;
    if (vn->type != VLNK || !vn->ops || !vn->ops->readlink) return -EINVAL;
    return vn->ops->readlink(vn, buf, size);
}

/* A socket's name in the file system: a node with no operations that
 * connect(2) finds by path (fs/unixsock.c keeps the socket itself). */
int vfs_mksock(const char *path, mode_t mode, struct vnode **out)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(NULL, path, WALK_PARENT, &parent, name);
    if (r < 0) return r;
    if (child_named(parent, name)) return -EADDRINUSE;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct vnode *s = vfs_node_new(parent->fs, name, VSOCK, mode & 0777, NULL);
    if (!s) return -ENOMEM;
    vfs_node_add(parent, s);                            /* in memory only, also on a disk */
    *out = s;
    inotify_event(parent, IN_CREATE, name, 0);
    return 0;
}

/* Find the entry `name` in `dir` (the covered node, not a mount root). */
static struct vnode *entry_named(struct vnode *dir, const char *name)
{
    if (dir->ops && dir->ops->refresh) dir->ops->refresh(dir);
    for (struct vnode *c = dir->children; c; c = c->sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static void node_destroy(struct vnode *vn)
{
    inotify_vnode_gone(vn);
    if (vn->ops && vn->ops->evict) vn->ops->evict(vn);
    if (vn->ops == &ramfs_ops && vn->type == VREG && vn->data) vmobj_put(vn->data);
    if (vn->ops == &ramfs_link_ops) kfree(vn->data);
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

static int remove_node(struct vnode *dir, struct vnode *vn)
{
    if (dir->ops && dir->ops->remove) {
        int r = dir->ops->remove(dir, vn);
        if (r < 0) return r;
    }
    inotify_event(dir, IN_DELETE | (vn->type == VDIR ? IN_ISDIR : 0), vn->name, 0);
    inotify_event(vn, IN_DELETE_SELF, NULL, 0);
    node_remove(dir, vn);
    node_free(vn);
    return 0;
}

/* unlink(2): anything but directories (symbolic links themselves) */
int vfs_unlink_at(struct vnode *base, const char *path)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(base, path, WALK_PARENT, &parent, name);
    if (r < 0) return r;
    struct vnode *vn = entry_named(parent, name);
    if (!vn) return -ENOENT;
    if (vn->type == VDIR) return -EISDIR;
    if ((r = check_writable_dir(parent)) < 0) return r;
    return remove_node(parent, vn);
}

int vfs_unlink(const char *path) { return vfs_unlink_at(NULL, path); }

/* rmdir(2): empty directories only, never a mount point or "." */
int vfs_rmdir_at(struct vnode *base, const char *path)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk(base, path, WALK_PARENT, &parent, name);
    if (r < 0) return r;
    if (!strcmp(name, ".")) return -EINVAL;
    if (!strcmp(name, "..")) return -ENOTEMPTY;
    struct vnode *vn = entry_named(parent, name);
    if (!vn) return -ENOENT;
    if (vn->type != VDIR) return -ENOTDIR;
    if (vn->mounted) return -EBUSY;
    if (vn->ops && vn->ops->refresh) vn->ops->refresh(vn);
    if (vn->children) return -ENOTEMPTY;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct tcb *t = current_task();
    if ((r = remove_node(parent, vn)) < 0) return r;
    if (t && t->fs && t->fs->cwd == vn) t->fs->cwd = parent;    /* don't leave a dangling cwd */
    return 0;
}

int vfs_rmdir(const char *path) { return vfs_rmdir_at(NULL, path); }

/* rename(2) within one mounted file system; -EXDEV across mounts. */
int vfs_rename_at(struct vnode *obase, const char *from, struct vnode *nbase, const char *to)
{
    struct vnode *op, *np; char oname[VFS_NAME_MAX], nname[VFS_NAME_MAX];
    int r = walk(obase, from, WALK_PARENT, &op, oname);
    if (r < 0) return r;
    if ((r = walk(nbase, to, WALK_PARENT, &np, nname)) < 0) return r;
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
        if (old->type == VDIR) {
            if (old->ops && old->ops->refresh) old->ops->refresh(old);
            if (old->children) return -ENOTEMPTY;
        }
    }
    if (op->ops && op->ops->rename && (r = op->ops->rename(op, vn, np, nname, old)) < 0) return r;
    if (old) { inotify_event(old, IN_DELETE_SELF, NULL, 0); node_remove(np, old); node_free(old); }
    node_remove(op, vn);
    strlcpy(vn->name, nname, sizeof vn->name);
    vfs_node_add(np, vn);
    uint32_t cookie = inotify_cookie(), isdir = vn->type == VDIR ? IN_ISDIR : 0;
    inotify_event(op, IN_MOVED_FROM | isdir, oname, cookie);
    inotify_event(np, IN_MOVED_TO | isdir, nname, cookie);
    inotify_event(vn, IN_MOVE_SELF, NULL, 0);
    return 0;
}

int vfs_rename(const char *from, const char *to) { return vfs_rename_at(NULL, from, NULL, to); }

/* link(2): a second name for a file, on file systems that have it. */
int vfs_link_at(struct vnode *obase, const char *from, bool follow, struct vnode *nbase, const char *to)
{
    struct vnode *vn, *parent, *nv; char name[VFS_NAME_MAX];
    int r = walk(obase, from, follow ? WALK_FOLLOW : 0, &vn, NULL);
    if (r < 0) return r;
    if (vn->type == VDIR) return -EPERM;
    if ((r = walk(nbase, to, WALK_PARENT, &parent, name)) < 0) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..") || child_named(parent, name)) return -EEXIST;
    if (parent->fs != vn->fs) return -EXDEV;
    if ((r = check_writable_dir(parent)) < 0) return r;
    if (!parent->ops || !parent->ops->link) return -EPERM;  /* the in-memory tree: one name per node */
    if ((r = parent->ops->link(parent, name, vn, &nv)) < 0) return r;
    strlcpy(nv->name, name, sizeof nv->name);
    vfs_node_add(parent, nv);
    inotify_event(parent, IN_CREATE, name, 0);
    inotify_self(vn, IN_ATTRIB);
    return 0;
}

static int attr_target(struct vnode *base, const char *path, bool follow, struct vnode **out)
{
    int r = walk(base, path, follow ? WALK_FOLLOW : 0, out, NULL);
    if (r < 0) return r;
    if ((*out)->fs && ((*out)->fs->flags & MNT_RDONLY)) return -EROFS;
    return 0;
}

int vfs_chmod_vn(struct vnode *vn, mode_t mode)
{
    if (vn->fs && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    if (vn->type == VLNK) return -EOPNOTSUPP;
    vn->mode = (vn->mode & ~07777u) | (mode & 07777u);
    return setattr(vn);
}

int vfs_chown_vn(struct vnode *vn, int uid, int gid)
{
    if (vn->fs && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    if (uid != -1) vn->uid = (uint32_t)uid;
    if (gid != -1) vn->gid = (uint32_t)gid;
    return setattr(vn);
}

int vfs_utime_vn(struct vnode *vn, int64_t mtime)
{
    if (vn->fs && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    vn->mtime = (uint64_t)(mtime < 0 ? time_realtime_sec() : mtime);
    return setattr(vn);
}

int vfs_chmod_at(struct vnode *base, const char *path, bool follow, mode_t mode)
{
    struct vnode *vn;
    int r = attr_target(base, path, follow, &vn);
    return r < 0 ? r : vfs_chmod_vn(vn, mode);
}

int vfs_chown_at(struct vnode *base, const char *path, bool follow, int uid, int gid)
{
    struct vnode *vn;
    int r = attr_target(base, path, follow, &vn);
    return r < 0 ? r : vfs_chown_vn(vn, uid, gid);
}

int vfs_utime_at(struct vnode *base, const char *path, bool follow, int64_t mtime)
{
    struct vnode *vn;
    int r = attr_target(base, path, follow, &vn);
    return r < 0 ? r : vfs_utime_vn(vn, mtime);
}

int vfs_chmod(const char *path, mode_t mode) { return vfs_chmod_at(NULL, path, true, mode); }
int vfs_chown(const char *path, int uid, int gid) { return vfs_chown_at(NULL, path, true, uid, gid); }
int vfs_utime(const char *path, int64_t mtime) { return vfs_utime_at(NULL, path, true, mtime); }

static uint64_t node_size(struct vnode *vn)
{
    if (vn->type == VDIR) return 0;
    return vn->ops && vn->ops->size ? vn->ops->size(vn) : vn->size;
}

void vfs_fill_stat(struct vnode *vn, struct stat *st)
{
    if (vn->ops && vn->ops->getattr) vn->ops->getattr(vn);
    memset(st, 0, sizeof *st);
    st->st_dev = vn->fs ? vn->fs->dev : 0;
    st->st_ino = vn->ino;
    st->st_nlink = vn->nlink ? vn->nlink : vn->type == VDIR ? 2 : 1;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_rdev = vn->rdev;
    st->st_size = (int64_t)node_size(vn);
    st->st_blksize = 4096;
    st->st_blocks = vn->blocks ? (int64_t)vn->blocks : (st->st_size + 511) / 512;
    st->st_mtim.tv_sec = (int64_t)vn->mtime;
    st->st_atim.tv_sec = (int64_t)(vn->atime ? vn->atime : vn->mtime);
    st->st_ctim.tv_sec = (int64_t)(vn->ctime ? vn->ctime : vn->mtime);
}

int vfs_stat_at(struct vnode *base, const char *path, bool follow, struct stat *st)
{
    struct vnode *vn;
    int r = walk(base, path, follow ? WALK_FOLLOW : 0, &vn, NULL);
    if (r < 0) return r;
    vfs_fill_stat(vn, st);
    return 0;
}

int vfs_stat(const char *path, struct stat *st) { return vfs_stat_at(NULL, path, true, st); }

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

int vfs_statfs(struct vnode *vn, struct kstatfs *st)
{
    memset(st, 0, sizeof *st);
    struct mount *m = vn->fs;
    st->f_bsize = st->f_frsize = 4096;
    st->f_namelen = VFS_NAME_MAX - 1;
    st->f_type = 0x858458f6;                            /* RAMFS_MAGIC unless the fs says else */
    if (m) { st->f_fsid[0] = (int32_t)m->dev; if (m->flags & MNT_RDONLY) st->f_flags |= 1; }
    if (m && m->statfs) return m->statfs(m, st);
    st->f_blocks = st->f_bfree = st->f_bavail = pmm_free_bytes() / 4096;   /* memory is the limit */
    return 0;
}

void vfs_sync_all(bool final)
{
    for (struct mount *m = mounts; m; m = m->next)
        if (m->sync && strcmp(m->fstype, "bind")) m->sync(m, final);
}

/* ======================================================================= */
/*  open file objects                                                        */
/* ======================================================================= */

static int open_node(struct vnode *vn, int flags, bool created, struct file **out);

int vfs_open_at(struct vnode *base, const char *path, int flags, mode_t mode, struct file **out)
{
    struct vnode *vn;
    bool follow = !(flags & O_NOFOLLOW);
    int r = walk(base, path, follow ? WALK_FOLLOW : 0, &vn, NULL);
    bool created = false;

    if (r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *parent; char name[VFS_NAME_MAX];
        if ((r = walk(base, path, WALK_PARENT, &parent, name)) < 0) return r;
        if (!strcmp(name, ".") || !strcmp(name, "..")) return -EISDIR;
        if (entry_named(parent, name)) return -EEXIST;  /* a dangling symlink: not followed */
        struct tcb *t = current_task();
        if ((r = create_node(parent, name, VREG, mode & 07777 & ~(t && t->fs ? t->fs->umask : 022), NULL, &vn)) < 0)
            return r;
        created = true;
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return -EEXIST;
    }
    return open_node(vn, flags, created, out);
}

/* Open a node already found (/proc/self/fd/N): as open() would, minus creation. */
int vfs_open_vnode(struct vnode *vn, int flags, struct file **out)
{
    return open_node(vn, flags & ~(O_CREAT | O_EXCL), false, out);
}

static int open_node(struct vnode *vn, int flags, bool created, struct file **out)
{
    int r;
    int acc = flags & O_ACCMODE;
    if (vn->type == VLNK && !(flags & O_PATH)) return -ELOOP;  /* O_NOFOLLOW on a link */
    if (vn->type == VSOCK && !(flags & O_PATH)) return -ENXIO;  /* sockets are connect()ed, not opened */
    if (vn->type == VDIR && acc != O_RDONLY) return -EISDIR;
    if ((flags & O_DIRECTORY) && vn->type != VDIR) return -ENOTDIR;
    if (flags & O_PATH) {                               /* a handle on the node only */
        struct file *f = vfs_file_new(vn, O_PATH | (flags & O_CLOEXEC));
        if (!f) return -ENOMEM;
        *out = f;
        return 0;
    }
    if (acc != O_RDONLY && (vn->fs->flags & MNT_RDONLY)) return -EROFS;
    /* POSIX: the creating open gets the access it asked for whatever the new
     * mode says; root (uid 0) is not limited by permission bits. */
    struct tcb *self = current_task();
    if (acc != O_RDONLY && !(vn->mode & 0222) && !created && self && self->uid != 0) return -EACCES;

    if ((flags & O_TRUNC) && vn->type == VREG && acc != O_RDONLY && vn->ops && vn->ops->truncate) {
        if ((r = vn->ops->truncate(vn, 0)) < 0) return r;
        vn->mtime = (uint64_t)time_realtime_sec();
    }
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

int vfs_open(const char *path, int flags, mode_t mode, struct file **out)
{
    return vfs_open_at(NULL, path, flags, mode, out);
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
    if (n > 0) { f->vn->mtime = (uint64_t)time_realtime_sec(); inotify_self(f->vn, IN_MODIFY); }
    return n;
}

ssize_t vfs_write(struct file *f, const void *buf, size_t len)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->vn->ops && f->vn->ops->fwrite) return f->vn->ops->fwrite(f, buf, len);
    if (!f->vn->ops || !f->vn->ops->write) return -EINVAL;
    if (f->flags & O_APPEND) f->off = node_size(f->vn);
    ssize_t n = f->vn->ops->write(f->vn, buf, len, f->off);
    if (n > 0) {
        f->off += (uint64_t)n;
        f->vn->mtime = (uint64_t)time_realtime_sec();
        if (f->vn->type == VREG) inotify_self(f->vn, IN_MODIFY);
    }
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
    switch (vn->type) {
    case VDIR:  return DT_DIR;
    case VCHR:  return DT_CHR;
    case VREG:  return DT_REG;
    case VLNK:  return DT_LNK;
    case VSOCK: return DT_SOCK;
    default:    return DT_UNKNOWN;
    }
}

/* getdents64: f->off is the index of the next entry ("." = 0, ".." = 1) */
int vfs_getdents(struct file *f, void *buf, size_t len)
{
    struct vnode *dir = f->vn;
    if (dir->type != VDIR) return -ENOTDIR;
    if (f->off == 0 && dir->ops && dir->ops->refresh) dir->ops->refresh(dir);
    size_t pos = 0;
    for (;;) {
        /* f->off is the dpos of the next entry to return: entries removed
         * or added meanwhile do not make readers skip or repeat others
         * (rm -r deletes while it reads) */
        uint64_t idx = f->off, next;
        const char *name; struct vnode *vn;
        if (idx == 0)      { name = ".";  vn = dir; next = 1; }
        else if (idx == 1) { name = ".."; vn = dir->parent ? dir->parent : dir; next = 2; }
        else {
            for (vn = dir->children; vn && vn->dpos < idx; vn = vn->sibling) {}
            if (!vn) break;
            name = vn->name;
            next = vn->dpos + 1;
        }
        size_t nlen = strlen(name);
        size_t reclen = (offsetof(struct linux_dirent64, d_name) + nlen + 1 + 7) & ~(size_t)7;
        if (pos + reclen > len) { if (pos == 0) return -EINVAL; break; }
        struct linux_dirent64 *d = (struct linux_dirent64 *)((uint8_t *)buf + pos);
        struct vnode *shown = vn->mounted ? vn->mounted : vn;
        d->d_ino = shown->ino;
        d->d_off = (int64_t)next;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = dtype(shown);
        memcpy(d->d_name, name, nlen + 1);
        pos += reclen;
        f->off = next;
    }
    return (int)pos;
}

int vfs_ioctl(struct file *f, unsigned long req, void *arg)
{
    if (f->vn->ops && f->vn->ops->fioctl) return f->vn->ops->fioctl(f, req, arg);
    if (!f->vn->ops || !f->vn->ops->ioctl) return -ENOTTY;
    return f->vn->ops->ioctl(f->vn, req, arg);
}

int vfs_fstat(struct file *f, struct stat *st) { vfs_fill_stat(f->vn, st); return 0; }

/* Reference counts change with locked instructions: descriptors are shared
 * between threads, duplicated by dup/fork and carried in SCM_RIGHTS
 * messages, and the last vfs_close() alone releases the object. */
struct file *vfs_file_get(struct file *f)
{
    if (f) __atomic_add_fetch(&f->refcnt, 1, __ATOMIC_ACQ_REL);
    return f;
}

/* ---- flock(2): advisory whole-file locks --------------------------------
 * A lock belongs to an open file description, as on Linux: dup'd and
 * inherited descriptors share it, and it goes away with the description's
 * last close. Shared locks coexist; an exclusive one excludes all others. */
#define MAX_FLOCKS 256
static struct flock_ent { struct vnode *vn; struct file *owner; bool excl; } flocks[MAX_FLOCKS];
static int flock_chan;

static void flock_drop(struct file *f)
{
    bool any = false;
    for (int i = 0; i < MAX_FLOCKS; i++)
        if (flocks[i].owner == f) { flocks[i].owner = NULL; flocks[i].vn = NULL; any = true; }
    if (any) wakeup(&flock_chan);
}

int vfs_flock(struct file *f, int op)
{
    enum { LOCK_SH = 1, LOCK_EX = 2, LOCK_NB = 4, LOCK_UN = 8 };
    int kind = op & ~LOCK_NB;
    if (kind != LOCK_SH && kind != LOCK_EX && kind != LOCK_UN) return -EINVAL;
    uint64_t fl = irq_save();
    if (kind == LOCK_UN) { flock_drop(f); irq_restore(fl); return 0; }
    for (;;) {
        bool blocked = false;
        int mine = -1, slot = -1;
        for (int i = 0; i < MAX_FLOCKS; i++) {
            if (!flocks[i].owner) { if (slot < 0) slot = i; continue; }
            if (flocks[i].vn != f->vn) continue;
            if (flocks[i].owner == f) { mine = i; continue; }
            if (kind == LOCK_EX || flocks[i].excl) blocked = true;
        }
        if (!blocked) {
            if (mine >= 0) flocks[mine].excl = kind == LOCK_EX;     /* conversion */
            else if (slot < 0) { irq_restore(fl); return -ENOLCK; }
            else flocks[slot] = (struct flock_ent){ f->vn, f, kind == LOCK_EX };
            if (mine >= 0 && kind == LOCK_SH) wakeup(&flock_chan);
            irq_restore(fl);
            return 0;
        }
        /* Linux drops a held lock before waiting to convert it, so two
         * holders upgrading at once cannot deadlock. */
        if (mine >= 0) { flocks[mine].owner = NULL; flocks[mine].vn = NULL; wakeup(&flock_chan); }
        if (op & LOCK_NB) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        sleep_on(&flock_chan);
    }
}

static void vfs_close_last(struct file *f);

void vfs_close(struct file *f)
{
    if (!f || __atomic_sub_fetch(&f->refcnt, 1, __ATOMIC_ACQ_REL) > 0) return;
    /* The last close runs file-system and driver code: under the big kernel
     * lock, also from a call that runs without it (unix socket I/O). */
    if (!bkl_depth()) { bkl_enter(); vfs_close_last(f); bkl_exit(); return; }
    vfs_close_last(f);
}

static void vfs_close_last(struct file *f)
{
    if (f->ep_links) epoll_file_closing(f);            /* leaves every epoll set */
    flock_drop(f);
    vfs_locks_file_gone(f);
    struct vnode *vn = f->vn;
    if (vn && (vn->type == VREG || vn->type == VDIR))
        inotify_self(vn, (f->flags & O_ACCMODE) != O_RDONLY ? IN_CLOSE_WRITE : IN_CLOSE_NOWRITE);
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

bool file_nolock(struct file *f) { return f && f->vn && f->vn->ops && f->vn->ops->nolock; }

uint64_t vfs_poll_deadline(struct file *f)
{
    return f->vn && f->vn->ops && f->vn->ops->poll_deadline ? f->vn->ops->poll_deadline(f) : 0;
}

/* Files without a poll op (regular files, most devices) never block. */
int vfs_poll(struct file *f, int events)
{
    if (f->vn && f->vn->ops && f->vn->ops->poll) {
        struct poll_table *pt = current_task() ? current_task()->poll_tab : NULL;
        uint32_t calls = pt ? pt->calls : 0;
        int r = f->vn->ops->poll(f, events);
        if (pt && pt->calls == calls) pt->all = true;   /* named no channel: any wakeup counts */
        return r;
    }
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

/* in-memory symbolic links: the target string in vn->data */
static int ramfs_readlink(struct vnode *vn, char *buf, size_t size)
{
    size_t n = strlen(vn->data);
    if (n > size) n = size;
    memcpy(buf, vn->data, n);
    return (int)n;
}

static const struct vnode_ops ramfs_link_ops = { .readlink = ramfs_readlink };

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
    if (!r) { vn->mtime = (uint64_t)time_realtime_sec(); inotify_self(vn, IN_MODIFY); }
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

/* Make the file system `m` "/". Each mount whose mount point was a
 * directory of the old root moves to the same path in the new one (the
 * directory is made if the new root lacks it); a bind mount whose
 * destination the new root already has is dropped (its own /bin wins over
 * /boot/bin). Working directories in the old root move to the new one.
 * The old root's nodes stay allocated: open files on them keep working. */
int vfs_pivot_root(struct mount *m)
{
    struct mount *oldm = root->fs;
    root = m->root;
    root->parent = root;
    m->covered = NULL;
    strlcpy(m->path, "/", sizeof m->path);
    for (struct mount **pp = &mounts; *pp; pp = &(*pp)->next)    /* the old root leaves the list */
        if (*pp == oldm) { *pp = oldm->next; break; }
    m->next = mounts;
    mounts = m;
    kprintf("vfs: %s is now the root file system\n", m->fstype);

    for (struct mount **pp = &m->next; *pp;) {
        struct mount *mt = *pp;
        if (!mt->covered || mt->covered->fs != oldm) { pp = &mt->next; continue; }
        bool bind = !strcmp(mt->fstype, "bind");
        struct vnode *d;
        int r = vfs_lookup(mt->path, &d);
        if (r == 0 && bind) {
            kprintf("vfs: %s: the new root has its own, bind dropped\n", mt->path);
            goto drop;
        }
        if (r == -ENOENT && (r = vfs_mkdir(mt->path, 0755)) == 0) r = vfs_lookup(mt->path, &d);
        if (r < 0 || d->type != VDIR || d->mounted || d == root) {
            kprintf("vfs: %s cannot move to the new root (%d): no longer reachable\n", mt->path, r);
            goto drop;
        }
        mt->covered->mounted = NULL;
        d->mounted = mt->root;
        mt->covered = d;
        if (!bind) { mt->root->parent = d->parent; strlcpy(mt->root->name, d->name, sizeof mt->root->name); }
        pp = &mt->next;
        continue;
drop:
        mt->covered->mounted = NULL;
        *pp = mt->next;
        kfree(mt);
    }
    for (int i = 0; i < MAX_TASKS; i++) {               /* nothing may stay behind in the old tree */
        struct tcb *t = task_slot(i);
        if (t && t->fs && t->fs->cwd && t->fs->cwd->fs == oldm) t->fs->cwd = root;
    }
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
    struct mount *shm;                                  /* POSIX shared memory: a plain ramfs */
    vfs_mkfs("tmpfs", 0, &shm)->mode = S_IFDIR | 01777;
    vfs_mount(shm, "/dev/shm");
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
    vfs_symlink_at("/proc/net/pnp", NULL, "/etc/resolv.conf");    /* name servers from DHCP or kestrel.ip */
    ramfs_write_file("/etc/motd",
        "Welcome to Kestrel -- a freestanding x86_64 POSIX-style kernel.\n"
        "Everything above was read through open/read/getdents64 system calls.\n");
}
