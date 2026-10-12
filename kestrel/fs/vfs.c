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
/*  the tree's lock and node references                                      */
/* ======================================================================= */

/* One reader-writer lock guards the tree: the directories' child lists,
 * names and parents, mount points, and working directories. Lookups,
 * stat, readlink and getdents hold it shared, so they run on several CPUs
 * at once and without the big kernel lock; creating, removing and renaming
 * hold it exclusively. A task may take it again while it holds it (the
 * count is per task), but never exclusively inside a shared hold.
 *
 * Order: the tree's lock, then a file system's own lock (ext2's volume
 * mutex), then the big kernel lock. A task holding the big lock that has
 * to wait for the tree sleeps, which gives the big lock up, so the
 * opposite order does not deadlock. */
static struct krwlock tree;

void vfs_tree_lock(bool excl)
{
    struct tcb *t = current_task();
    if (!t) return;                                     /* early boot: one thread */
    if (t->vfs_depth++) {
        if (excl && !t->vfs_excl) panic("vfs: the tree's lock taken exclusively inside a shared hold");
        return;
    }
    t->vfs_excl = excl;
    if (excl) krw_wlock(&tree); else krw_rlock(&tree);
}

void vfs_tree_unlock(void)
{
    struct tcb *t = current_task();
    if (!t || --t->vfs_depth) return;
    if (t->vfs_excl) krw_wunlock(&tree); else krw_runlock(&tree);
    t->vfs_excl = false;
}

static bool tree_excl(void) { struct tcb *t = current_task(); return !t || t->vfs_excl; }

/* A node's `refs` counts open files, lookups in progress and working
 * directories. VN_GONE is set in it when the node leaves the tree; the node
 * is freed when both are true: no reference left and out of the tree. */
#define VN_GONE (1 << 30)

static void node_destroy(struct vnode *vn);

void vfs_vnode_get(struct vnode *vn)
{
    if (vn) __atomic_add_fetch(&vn->refs, 1, __ATOMIC_ACQ_REL);
}

/* Drop a reference; true if it was the last one of a node out of the tree
 * (the caller frees it with node_destroy). */
static bool vnode_unref(struct vnode *vn)
{
    return vn && __atomic_sub_fetch(&vn->refs, 1, __ATOMIC_ACQ_REL) == VN_GONE;
}

void vfs_vnode_put(struct vnode *vn)
{
    if (vnode_unref(vn)) node_destroy(vn);
}

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
    vn->ino   = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    vn->ops   = ops;
    vn->fs    = fs;
    vn->mtime = (uint64_t)time_realtime_sec();
    return vn;
}

/* Append child to dir. Callers hold the tree's lock exclusively, or run at
 * boot, or (a directory's refresh, see vnode_ops) hold it shared and are the
 * only task adding to that directory: lookups on other CPUs may be walking
 * the list, so the new node is complete before it is linked in. */
void vfs_node_add(struct vnode *dir, struct vnode *child)
{
    child->parent = dir;
    child->sibling = NULL;
    if (dir->dnext < 2) dir->dnext = 2;                 /* 0 and 1 are "." and ".." */
    child->dpos = dir->dnext++;                         /* stable while others come and go */
    struct vnode **pp = &dir->children;         /* append: keeps creation order */
    while (*pp) pp = &(*pp)->sibling;
    __atomic_store_n(pp, child, __ATOMIC_RELEASE);
}

static struct vnode *first_child(struct vnode *dir) { return __atomic_load_n(&dir->children, __ATOMIC_ACQUIRE); }
static struct vnode *next_child(struct vnode *c) { return __atomic_load_n(&c->sibling, __ATOMIC_ACQUIRE); }

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
    m->dev = __atomic_fetch_add(&next_dev, 1, __ATOMIC_RELAXED);
    m->root = vfs_node_new(m, "", VDIR, 0755, &ramfs_ops);
    *out = m;
    return m->root;
}

#define WALK_FOLLOW 1           /* follow a symbolic link in the last component */
#define WALK_PARENT 2           /* stop before the last component, return its name */
static int walk(struct vnode *base, const char *path, int flags, struct vnode **out, char *last);

int vfs_mount(struct mount *m, const char *path)
{
    vfs_tree_lock(true);
    if (!root) {                                    /* the root filesystem */
        root = m->root;
        root->parent = root;
        strlcpy(m->path, "/", sizeof m->path);
    } else {
        struct vnode *dir;
        int r = walk(NULL, path, WALK_FOLLOW, &dir, NULL);
        if (r < 0) { vfs_tree_unlock(); return r; }
        if (dir->type != VDIR) { vfs_tree_unlock(); return -ENOTDIR; }
        if (dir->mounted) { vfs_tree_unlock(); return -EBUSY; }
        dir->mounted = m->root;
        m->covered = dir;
        m->root->parent = dir->parent;
        strlcpy(m->root->name, dir->name, sizeof m->root->name);
        strlcpy(m->path, path, sizeof m->path);
    }
    struct mount **pp = &mounts;
    while (*pp) pp = &(*pp)->next;
    *pp = m;
    vfs_tree_unlock();
    kprintf("vfs: mounted %-6s on %s%s\n", m->fstype, m->path, (m->flags & MNT_RDONLY) ? " (ro)" : "");
    return 0;
}

/* ======================================================================= */
/*  path resolution                                                          */
/* ======================================================================= */

/* The working directory. Under the tree's lock: chdir changes it
 * exclusively, so it holds still while the lock is held. */
static struct vnode *cwd_node(void)
{
    struct tcb *t = current_task();
    return (t && t->fs && t->fs->cwd) ? t->fs->cwd : root;
}

struct vnode *vfs_cwd_get(struct fs_struct *fs)
{
    vfs_tree_lock(false);
    struct vnode *vn = fs && fs->cwd ? fs->cwd : root;
    vfs_vnode_get(vn);
    vfs_tree_unlock();
    return vn;
}

/* Make vn (a directory) the calling task's working directory. */
void vfs_set_cwd(struct vnode *vn)
{
    struct tcb *t = current_task();
    if (!t || !t->fs) return;
    vfs_vnode_get(vn);
    vfs_tree_lock(true);
    struct vnode *old = t->fs->cwd;
    t->fs->cwd = vn;
    vfs_tree_unlock();
    vfs_vnode_put(old);
}

#define TREE_RETRY (-4095)      /* a directory needs the exclusive lock to refresh */

/* Bring dir's children up to date (see vnode_ops.refresh). TREE_RETRY:
 * the caller holds the lock shared and must start over exclusively. A
 * caller inside someone else's shared hold makes do with the list as it is. */
static int refresh(struct vnode *dir)
{
    if (!dir->ops || !dir->ops->refresh) return 0;
    if (dir->ops->stale) {
        if (!dir->ops->stale(dir)) return 0;
        if (!tree_excl()) return current_task()->vfs_depth > 1 ? 0 : TREE_RETRY;
    }
    dir->ops->refresh(dir);
    return 0;
}

/* The entry `name` of dir, after a refresh: a mount point as the mounted
 * root (cross), or as the covered directory itself. */
static int find(struct vnode *dir, const char *name, bool cross, struct vnode **out)
{
    int r = refresh(dir);
    if (r < 0) return r;
    for (struct vnode *c = first_child(dir); c; c = next_child(c))
        if (strcmp(c->name, name) == 0) { *out = cross && c->mounted ? c->mounted : c; return 0; }
    return -ENOENT;
}

/* find() where the lock is held exclusively (no TREE_RETRY): NULL if none. */
static struct vnode *entry_named(struct vnode *dir, const char *name)
{
    struct vnode *c;
    return find(dir, name, false, &c) == 0 ? c : NULL;
}

/* The target of symbolic link vn into buf (NUL-terminated); its length. */
static int link_target(struct vnode *vn, char *buf, size_t size)
{
    if (vn->type != VLNK || !vn->ops || !vn->ops->readlink) return -EINVAL;
    bool bkl = !vn->ops->nolock && !bkl_depth();        /* /proc's links read other processes */
    if (bkl) bkl_enter();
    int n = vn->ops->readlink(vn, buf, size - 1);
    if (bkl) bkl_exit();
    if (n < 0) return n;
    buf[n] = 0;
    return n;
}


/* Resolve `path` from `base` (NULL: the cwd), with the tree's lock held.
 * WALK_PARENT: return the directory holding the last component in *out and
 * copy that component into `last` (VFS_NAME_MAX bytes); the last component
 * itself is not looked up. Symbolic links in the middle of the path are
 * always followed: the rest of the path is appended to the link's target
 * and the walk goes on from "/" or from the link's directory. The node
 * found has no reference of its own: it holds still while the lock is held. */
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
        if (!strcmp(comp, "..")) {
            if (vn->unlinked) { r = -ENOENT; goto out; }    /* a removed directory has no parent */
            vn = vn->parent ? vn->parent : root;
            p = e;
            continue;
        }
        struct vnode *c;
        if ((r = find(vn, comp, true, &c)) < 0) goto out;
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

/* walk() under the tree's lock: shared (exclusive if excl), or exclusive
 * after all if a directory on the way asks for it to refresh (/proc). The
 * lock is held on return, whatever the result. */
static int walk_locked(struct vnode *base, const char *path, int flags, struct vnode **out, char *last, bool excl)
{
    for (;;) {
        vfs_tree_lock(excl);
        int r = walk(base, path, flags, out, last);
        if (r != TREE_RETRY) return r;
        vfs_tree_unlock();
        excl = true;
    }
}

/* Look a path up and take a reference on the node (vfs_vnode_put). */
static int lookup_ref(struct vnode *base, const char *path, int flags, struct vnode **out)
{
    int r = walk_locked(base, path, flags, out, NULL, false);
    if (r == 0) vfs_vnode_get(*out);
    vfs_tree_unlock();
    return r;
}

int vfs_lookup_at(struct vnode *base, const char *path, bool follow, struct vnode **out)
{
    return lookup_ref(base, path, follow ? WALK_FOLLOW : 0, out);
}

int vfs_lookup(const char *path, struct vnode **out) { return lookup_ref(NULL, path, WALK_FOLLOW, out); }

/* vn's absolute path, with the tree's lock held. A node out of the tree
 * (unlinked, or a memfd) is "/name (deleted)", as on Linux. */
static int path_of(struct vnode *vn, char *buf, size_t size)
{
    if (vn && vn->unlinked) {
        int n = snprintf(buf, size, "/%s (deleted)", vn->name);
        return n < 0 || (size_t)n >= size ? -ERANGE : 0;
    }
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

int vfs_path_of(struct vnode *vn, char *buf, size_t size)
{
    vfs_tree_lock(false);
    int r = path_of(vn, buf, size);
    vfs_tree_unlock();
    return r;
}

static int check_writable_dir(struct vnode *dir)
{
    if (dir->unlinked) return -ENOENT;                  /* nothing is made in a removed directory */
    if (dir->fs->flags & MNT_RDONLY) return -EROFS;
    if (dir->fs->flags & MNT_NOCREATE) return -EPERM;
    return 0;
}

static const struct vnode_ops ramfs_link_ops;

/* A new entry `name` in `dir`: through the file system's create operation
 * if it has one, else an in-memory node. Owned by the caller's uid/gid.
 * The tree's lock is held exclusively. */
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

static mode_t umask_of_caller(void)
{
    struct tcb *t = current_task();
    return t && t->fs ? t->fs->umask : 022;
}

int vfs_mkdir_at(struct vnode *base, const char *path, mode_t mode)
{
    struct vnode *parent, *d; char name[VFS_NAME_MAX];
    int r = walk_locked(base, path, WALK_PARENT, &parent, name, true);
    if (r == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || entry_named(parent, name)) r = -EEXIST;
        else r = create_node(parent, name, VDIR, mode & 07777 & ~umask_of_caller(), NULL, &d);
    }
    vfs_tree_unlock();
    return r;
}

int vfs_mkdir(const char *path, mode_t mode) { return vfs_mkdir_at(NULL, path, mode); }

int vfs_symlink_at(const char *target, struct vnode *base, const char *path)
{
    struct vnode *parent, *l; char name[VFS_NAME_MAX];
    if (!target || !*target) return -ENOENT;
    if (strlen(target) >= VFS_PATH_MAX) return -ENAMETOOLONG;
    int r = walk_locked(base, path, WALK_PARENT, &parent, name, true);
    if (r == 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..") || entry_named(parent, name)) r = -EEXIST;
        else r = create_node(parent, name, VLNK, 0777, target, &l);
    }
    vfs_tree_unlock();
    return r;
}

int vfs_readlink_at(struct vnode *base, const char *path, char *buf, size_t size)
{
    struct vnode *vn;
    int r = lookup_ref(base, path, 0, &vn);
    if (r < 0) return r;
    if (vn->type != VLNK || !vn->ops || !vn->ops->readlink) r = -EINVAL;
    else {
        bool bkl = !vn->ops->nolock && !bkl_depth();
        if (bkl) bkl_enter();
        r = vn->ops->readlink(vn, buf, size);
        if (bkl) bkl_exit();
    }
    vfs_vnode_put(vn);
    return r;
}

/* A socket's name in the file system: a node with no operations that
 * connect(2) finds by path (fs/unixsock.c keeps the socket itself). */
int vfs_mksock(const char *path, mode_t mode, struct vnode **out)
{
    struct vnode *parent; char name[VFS_NAME_MAX];
    int r = walk_locked(NULL, path, WALK_PARENT, &parent, name, true);
    if (r == 0 && entry_named(parent, name)) r = -EADDRINUSE;
    if (r == 0) r = check_writable_dir(parent);
    struct vnode *s = NULL;
    if (r == 0 && !(s = vfs_node_new(parent->fs, name, VSOCK, mode & 0777, NULL))) r = -ENOMEM;
    if (r == 0) {
        vfs_node_add(parent, s);                        /* in memory only, also on a disk */
        *out = s;
        inotify_event(parent, IN_CREATE, name, 0);
    }
    vfs_tree_unlock();
    return r;
}

static void node_destroy(struct vnode *vn)
{
    inotify_vnode_gone(vn);
    if (vn->ops && vn->ops->evict) vn->ops->evict(vn);
    if (vn->ops == &ramfs_ops && vn->type == VREG && vn->data) vmobj_put(vn->data);
    if (vn->ops == &ramfs_link_ops) kfree(vn->data);
    kfree(vn);
}

/* A node leaves the tree (the lock is held exclusively). Like Linux, a file
 * that is still open stays alive (reads, writes and mappings keep working)
 * until its last close; so does a lookup's or a working directory's node. */
static void node_free(struct vnode *vn)
{
    vn->unlinked = true;
    vn->parent = NULL;
    if ((__atomic_fetch_or(&vn->refs, VN_GONE, __ATOMIC_ACQ_REL) & ~VN_GONE) == 0) node_destroy(vn);
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
    int r = walk_locked(base, path, WALK_PARENT, &parent, name, true);
    if (r == 0) {
        struct vnode *vn = entry_named(parent, name);
        if (!vn) r = -ENOENT;
        else if (vn->type == VDIR) r = -EISDIR;
        else if ((r = check_writable_dir(parent)) == 0) r = remove_node(parent, vn);
    }
    vfs_tree_unlock();
    return r;
}

int vfs_unlink(const char *path) { return vfs_unlink_at(NULL, path); }

/* rmdir(2): empty directories only, never a mount point or "." */
static int rmdir_locked(struct vnode *base, const char *path)
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
    refresh(vn);
    if (vn->children) return -ENOTEMPTY;
    if ((r = check_writable_dir(parent)) < 0) return r;
    struct tcb *t = current_task();
    vfs_vnode_get(vn);
    if ((r = remove_node(parent, vn)) == 0 && t && t->fs && t->fs->cwd == vn) {
        vfs_vnode_get(parent);                          /* don't leave the caller in a removed directory */
        t->fs->cwd = parent;
        vfs_vnode_put(vn);                              /* the working directory's reference */
    }
    vfs_vnode_put(vn);
    return r;
}

int vfs_rmdir_at(struct vnode *base, const char *path)
{
    vfs_tree_lock(true);
    int r = rmdir_locked(base, path);
    vfs_tree_unlock();
    return r;
}

int vfs_rmdir(const char *path) { return vfs_rmdir_at(NULL, path); }

/* rename(2) within one mounted file system; -EXDEV across mounts. */
static int rename_locked(struct vnode *obase, const char *from, struct vnode *nbase, const char *to)
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
            refresh(old);
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

int vfs_rename_at(struct vnode *obase, const char *from, struct vnode *nbase, const char *to)
{
    vfs_tree_lock(true);
    int r = rename_locked(obase, from, nbase, to);
    vfs_tree_unlock();
    return r;
}

int vfs_rename(const char *from, const char *to) { return vfs_rename_at(NULL, from, NULL, to); }

/* link(2): a second name for a file, on file systems that have it. */
static int link_locked(struct vnode *obase, const char *from, bool follow, struct vnode *nbase, const char *to)
{
    struct vnode *vn, *parent, *nv; char name[VFS_NAME_MAX];
    int r = walk(obase, from, follow ? WALK_FOLLOW : 0, &vn, NULL);
    if (r < 0) return r;
    if (vn->type == VDIR) return -EPERM;
    if ((r = walk(nbase, to, WALK_PARENT, &parent, name)) < 0) return r;
    if (!strcmp(name, ".") || !strcmp(name, "..") || entry_named(parent, name)) return -EEXIST;
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

int vfs_link_at(struct vnode *obase, const char *from, bool follow, struct vnode *nbase, const char *to)
{
    vfs_tree_lock(true);
    int r = link_locked(obase, from, follow, nbase, to);
    vfs_tree_unlock();
    return r;
}

/* The node of a path for an attribute change, with a reference. */
static int attr_target(struct vnode *base, const char *path, bool follow, struct vnode **out)
{
    int r = lookup_ref(base, path, follow ? WALK_FOLLOW : 0, out);
    if (r < 0) return r;
    if ((*out)->fs && ((*out)->fs->flags & MNT_RDONLY)) { vfs_vnode_put(*out); return -EROFS; }
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
    if (r < 0) return r;
    r = vfs_chmod_vn(vn, mode);
    vfs_vnode_put(vn);
    return r;
}

int vfs_chown_at(struct vnode *base, const char *path, bool follow, int uid, int gid)
{
    struct vnode *vn;
    int r = attr_target(base, path, follow, &vn);
    if (r < 0) return r;
    r = vfs_chown_vn(vn, uid, gid);
    vfs_vnode_put(vn);
    return r;
}

int vfs_utime_at(struct vnode *base, const char *path, bool follow, int64_t mtime)
{
    struct vnode *vn;
    int r = attr_target(base, path, follow, &vn);
    if (r < 0) return r;
    r = vfs_utime_vn(vn, mtime);
    vfs_vnode_put(vn);
    return r;
}

int vfs_chmod(const char *path, mode_t mode) { return vfs_chmod_at(NULL, path, true, mode); }
int vfs_chown(const char *path, int uid, int gid) { return vfs_chown_at(NULL, path, true, uid, gid); }
int vfs_utime(const char *path, int64_t mtime) { return vfs_utime_at(NULL, path, true, mtime); }

static uint64_t node_size(struct vnode *vn)
{
    if (vn->type == VDIR) return 0;
    return vn->ops && vn->ops->size ? vn->ops->size(vn) : vn->size;
}

/* Generated files' sizes and devices' read other kernel state: those take
 * the big lock (a file system's own operations lock themselves). */
void vfs_fill_stat(struct vnode *vn, struct stat *st)
{
    bool bkl = vn->ops && !vn->ops->nolock && (vn->ops->getattr || vn->ops->size) && !bkl_depth();
    if (bkl) bkl_enter();
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
    if (bkl) bkl_exit();
}

int vfs_stat_at(struct vnode *base, const char *path, bool follow, struct stat *st)
{
    struct vnode *vn;
    int r = lookup_ref(base, path, follow ? WALK_FOLLOW : 0, &vn);
    if (r < 0) return r;
    vfs_fill_stat(vn, st);
    vfs_vnode_put(vn);
    return 0;
}

int vfs_stat(const char *path, struct stat *st) { return vfs_stat_at(NULL, path, true, st); }

int vfs_chdir(const char *path)
{
    struct vnode *vn;
    int r = vfs_lookup(path, &vn);
    if (r < 0) return r;
    if (vn->type != VDIR) r = -ENOTDIR;
    else vfs_set_cwd(vn);
    vfs_vnode_put(vn);
    return r;
}

int vfs_getcwd(char *buf, size_t size)
{
    vfs_tree_lock(false);
    int r = path_of(cwd_node(), buf, size);
    vfs_tree_unlock();
    return r;
}

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

/* Find or create the file to open, with a reference: the tree's lock is
 * taken shared to look, and exclusively only to create. */
static int open_target(struct vnode *base, const char *path, int flags, mode_t mode, struct vnode **out, bool *created)
{
    int wf = (flags & O_NOFOLLOW) ? 0 : WALK_FOLLOW;
    bool excl = false;
    for (;;) {
        int r = walk_locked(base, path, wf, out, NULL, excl);
        if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL)) r = -EEXIST;
        if (r != -ENOENT || !(flags & O_CREAT) || excl) {
            if (r == -ENOENT && excl) {                 /* still missing, now with the right to create */
                struct vnode *parent; char name[VFS_NAME_MAX];
                if ((r = walk(base, path, WALK_PARENT, &parent, name)) == 0) {
                    if (!strcmp(name, ".") || !strcmp(name, "..")) r = -EISDIR;
                    else if (entry_named(parent, name)) r = -EEXIST;   /* a dangling symlink: not followed */
                    else if ((r = create_node(parent, name, VREG, mode & 07777 & ~umask_of_caller(), NULL, out)) == 0)
                        *created = true;
                }
            }
            if (r == 0) vfs_vnode_get(*out);
            vfs_tree_unlock();
            return r;
        }
        vfs_tree_unlock();                              /* not there: look again, ready to create */
        excl = true;
    }
}

int vfs_open_at(struct vnode *base, const char *path, int flags, mode_t mode, struct file **out)
{
    struct vnode *vn;
    bool created = false;
    int r = open_target(base, path, flags, mode, &vn, &created);
    if (r < 0) return r;
    r = open_node(vn, flags, created, out);
    vfs_vnode_put(vn);                                  /* the open file has its own */
    return r;
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

    bool trunc = (flags & O_TRUNC) && vn->type == VREG && acc != O_RDONLY && vn->ops && vn->ops->truncate;
    /* a device's open, or a truncation, by code that is not known to lock
     * itself: under the big lock */
    bool bkl = vn->ops && !vn->ops->nolock && (vn->ops->open || trunc) && !bkl_depth();
    if (bkl) bkl_enter();
    if (trunc) {
        if ((r = vn->ops->truncate(vn, 0)) < 0) goto out;
        vn->mtime = (uint64_t)time_realtime_sec();
    }
    if (vn->ops && vn->ops->open) {
        r = vn->ops->open(vn, flags, out);
        if (r == 0) vfs_vnode_get((*out)->vn);
        goto out;
    }
    struct file *f = kzalloc(sizeof *f);
    if (!f) { r = -ENOMEM; goto out; }
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    vfs_vnode_get(vn);
    *out = f;
    r = 0;
out:
    if (bkl) bkl_exit();
    return r;
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
    /* Threads sharing a regular file read it in turn. Not other files: a read
     * of a terminal can block, and fd 0 and fd 1 are often one file. */
    bool reg = f->vn->type == VREG;
    if (reg) kmutex_lock(&f->pos_lock);
    ssize_t n = f->vn->ops->read(f->vn, buf, len, f->off);
    if (n > 0) f->off += (uint64_t)n;
    if (reg) kmutex_unlock(&f->pos_lock);
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
    bool reg = f->vn->type == VREG;
    if (reg) kmutex_lock(&f->pos_lock);
    /* O_APPEND on a file that locks itself: the file system finds the end
     * under its own lock, so two appenders never write over each other */
    bool append = (f->flags & O_APPEND) && reg && f->vn->ops->nolock;
    if (f->flags & O_APPEND && !append) f->off = node_size(f->vn);
    ssize_t n = f->vn->ops->write(f->vn, buf, len, append ? VFS_APPEND : f->off);
    if (n > 0) f->off = append ? node_size(f->vn) : f->off + (uint64_t)n;
    if (reg) kmutex_unlock(&f->pos_lock);
    if (n > 0) {
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
    /* In turn with reads and writes, or with getdents of a directory: the
     * position lock */
    bool locked = f->vn->type == VREG || f->vn->type == VDIR;
    if (locked) kmutex_lock(&f->pos_lock);
    if (whence == SEEK_CUR) base = (int64_t)f->off;
    off_t r = base + off < 0 ? -EINVAL : (off_t)(f->off = (uint64_t)(base + off));
    if (locked) kmutex_unlock(&f->pos_lock);
    return r;
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

/* getdents64: f->off is the index of the next entry ("." = 0, ".." = 1).
 * Under the file's position lock and the tree's lock, shared. */
int vfs_getdents(struct file *f, void *buf, size_t len)
{
    struct vnode *dir = f->vn;
    if (dir->type != VDIR) return -ENOTDIR;
    kmutex_lock(&f->pos_lock);
    for (bool excl = false;; excl = true) {
        vfs_tree_lock(excl);
        if (f->off != 0 || refresh(dir) != TREE_RETRY) break;
        vfs_tree_unlock();                              /* /proc: changes its list exclusively */
    }
    size_t pos = 0;
    int ret = 0;
    for (;;) {
        /* f->off is the dpos of the next entry to return: entries removed
         * or added meanwhile do not make readers skip or repeat others
         * (rm -r deletes while it reads) */
        uint64_t idx = f->off, next;
        const char *name; struct vnode *vn;
        if (idx == 0)      { name = ".";  vn = dir; next = 1; }
        else if (idx == 1) { name = ".."; vn = dir->parent ? dir->parent : dir; next = 2; }
        else {
            for (vn = first_child(dir); vn && vn->dpos < idx; vn = next_child(vn)) {}
            if (!vn) break;
            name = vn->name;
            next = vn->dpos + 1;
        }
        size_t nlen = strlen(name);
        size_t reclen = (offsetof(struct linux_dirent64, d_name) + nlen + 1 + 7) & ~(size_t)7;
        if (pos + reclen > len) { if (pos == 0) ret = -EINVAL; break; }
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
    vfs_tree_unlock();
    kmutex_unlock(&f->pos_lock);
    return ret < 0 ? ret : (int)pos;
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
    bool last = vnode_unref(vn);                        /* the last reference to an unlinked file */
    if (vn && vn->ops && vn->ops->release) vn->ops->release(f);    /* may free a socket's vnode */
    kfree(f);
    if (last) node_destroy(vn);
}

struct file *vfs_file_new(struct vnode *vn, int flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = vn; f->flags = flags; f->refcnt = 1;
    vfs_vnode_get(vn);
    return f;
}

/* A node without operations (a /proc or /sys directory, a socket's name)
 * has nothing that could need the big lock. */
bool file_nolock(struct file *f) { return f && f->vn && (!f->vn->ops || f->vn->ops->nolock); }

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
 * so shared mappings of the file and read()/write() stay coherent. The
 * object locks itself: reads and writes run without the big kernel lock
 * (vnode_ops.nolock). */
static ssize_t ramfs_read(struct vnode *vn, void *buf, size_t len, uint64_t off)
{
    struct vm_object *o = __atomic_load_n((struct vm_object **)&vn->data, __ATOMIC_ACQUIRE);
    return o ? vmobj_read(o, buf, len, off) : 0;
}

static struct vm_object *ramfs_vmobject(struct vnode *vn)
{
    if (vn->type != VREG) return NULL;
    struct vm_object *o = __atomic_load_n((struct vm_object **)&vn->data, __ATOMIC_ACQUIRE);
    if (o) return o;
    struct vm_object *n = vmobj_new(0), *none = NULL;
    if (!n) return NULL;
    if (__atomic_compare_exchange_n((struct vm_object **)&vn->data, &none, n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return n;
    vmobj_put(n);                                       /* another CPU made it first */
    return none;
}

static ssize_t ramfs_write(struct vnode *vn, const void *buf, size_t len, uint64_t off)
{
    struct vm_object *o = ramfs_vmobject(vn);
    if (!o) return -ENOSPC;
    kmutex_lock(&o->wlock);                             /* the end holds still for O_APPEND */
    ssize_t n;
    if (off == VFS_APPEND) off = o->size;
    if (o->seals & 0x0008 && len) n = -EPERM;           /* F_SEAL_WRITE */
    else if ((o->seals & 0x0004) && off + len > o->size) n = -EPERM;   /* F_SEAL_GROW */
    else n = vmobj_write(o, buf, len, off);
    if (n > 0) vn->size = o->size;
    kmutex_unlock(&o->wlock);
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
    kmutex_lock(&o->wlock);
    int r;
    if ((o->seals & 0x0002) && size < o->size) r = -EPERM;      /* F_SEAL_SHRINK */
    else if ((o->seals & 0x0004) && size > o->size) r = -EPERM; /* F_SEAL_GROW */
    else r = vmobj_truncate(o, size);
    if (!r) vn->size = size;
    kmutex_unlock(&o->wlock);
    return r;
}

const struct vnode_ops ramfs_ops = { .read = ramfs_read, .write = ramfs_write, .size = ramfs_size,
                                     .truncate = ramfs_truncate, .vmobject = ramfs_vmobject, .nolock = true };

/* in-memory symbolic links: the target string in vn->data */
static int ramfs_readlink(struct vnode *vn, char *buf, size_t size)
{
    size_t n = strlen(vn->data);
    if (n > size) n = size;
    memcpy(buf, vn->data, n);
    return (int)n;
}

static const struct vnode_ops ramfs_link_ops = { .readlink = ramfs_readlink, .nolock = true };   /* fixed text */

/* An open file on a ramfs node that is in no directory (memfd_create):
 * freed with its last close. */
struct file *ramfs_anon_file(const char *name, int flags)
{
    struct vnode *vn = vfs_node_new(root ? root->fs : NULL, name, VREG, 0600, &ramfs_ops);
    if (!vn) return NULL;
    vn->unlinked = true;                                /* in no directory: freed with the last close */
    vn->refs = VN_GONE;
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
static int bind_locked(const char *src, const char *dst)
{
    struct vnode *s, *d;
    int r;
    if ((r = walk(NULL, src, WALK_FOLLOW, &s, NULL)) < 0 || (r = walk(NULL, dst, WALK_FOLLOW, &d, NULL)) < 0) return r;
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

int vfs_bind(const char *src, const char *dst)
{
    vfs_tree_lock(true);
    int r = bind_locked(src, dst);
    vfs_tree_unlock();
    return r;
}

/* Make the file system `m` "/". Each mount whose mount point was a
 * directory of the old root moves to the same path in the new one (the
 * directory is made if the new root lacks it); a bind mount whose
 * destination the new root already has is dropped (its own /bin wins over
 * /boot/bin). Working directories in the old root move to the new one.
 * The old root's nodes stay allocated: open files on them keep working. */
static int pivot_locked(struct mount *m);

int vfs_pivot_root(struct mount *m)
{
    vfs_tree_lock(true);
    int r = pivot_locked(m);
    vfs_tree_unlock();
    return r;
}

static int pivot_locked(struct mount *m)
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
        int r = walk(NULL, mt->path, WALK_FOLLOW, &d, NULL);
        if (r == 0 && bind) {
            kprintf("vfs: %s: the new root has its own, bind dropped\n", mt->path);
            goto drop;
        }
        if (r == -ENOENT && (r = vfs_mkdir(mt->path, 0755)) == 0) r = walk(NULL, mt->path, WALK_FOLLOW, &d, NULL);
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
        if (t && t->fs && t->fs->cwd && t->fs->cwd->fs == oldm) {
            struct vnode *old = t->fs->cwd;
            vfs_vnode_get(root);
            t->fs->cwd = root;
            vfs_vnode_put(old);
        }
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
    if (vfs_lookup("/tmp", &tmp) == 0) { tmp->mode |= 01777; vfs_vnode_put(tmp); }

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
        "if (eval 'x=a; : \"${x//a/b}\"') 2>/dev/null; then    # bash, BusyBox ash: prompt escapes\n"
        "    PS1='\\[\\e[92m\\]\\u@\\h\\[\\e[0m\\]:\\[\\e[94m\\]\\w\\[\\e[0m\\]\\$ '\n"
        "else                                                # dash: none, but $PWD expands\n"
        "    e=$(printf '\\033'); p='$'; [ \"$(id -u)\" = 0 ] && p='#'\n"
        "    PS1=\"$e[92m$(id -un)@$(hostname 2>/dev/null || cat /etc/hostname)$e[0m:$e[94m\"'$PWD'\"$e[0m$p \"\n"
        "    unset e p\n"
        "fi\n"
        "export PS1\n"
        "cd \"$HOME\" 2>/dev/null\n"
        "echo \"$(uname -sr) on $(tty). Ctrl+Alt+F1..F12: other monitors; startx: the desktop.\"\n");
    vfs_symlink_at("/proc/net/pnp", NULL, "/etc/resolv.conf");    /* name servers from DHCP or kestrel.ip */
    ramfs_write_file("/etc/motd",
        "Welcome to Kestrel -- a freestanding x86_64 POSIX-style kernel.\n"
        "Everything above was read through open/read/getdents64 system calls.\n");
}
