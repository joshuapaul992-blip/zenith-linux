/* fs/lockf.c -- fcntl() byte-range locks: POSIX (F_SETLK) and open file description (F_OFD_SETLK)
 *
 * Advisory read and write locks on ranges of a file. A POSIX lock belongs
 * to the process and goes away when the process closes any descriptor of
 * the file, or exits; an OFD lock belongs to the open file and goes away
 * when its last descriptor closes. A process's own locks never conflict
 * with each other: a new lock replaces whatever of its own it overlaps
 * (splitting what is left over). SQLite (Firefox's profile databases) and
 * Firefox's profile lock use these. */
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define END_MAX UINT64_MAX
#define POSIX_OWNER(tgid) ((uintptr_t)(uint32_t)(tgid) | (1ull << 63))

struct rlock {
    struct rlock *next;
    struct vnode *vn;
    uintptr_t     owner;            /* POSIX_OWNER(tgid), or the struct file of an OFD lock */
    int           pid;              /* reported by F_GETLK (-1 for OFD locks) */
    int           type;             /* F_RDLCK, F_WRLCK */
    uint64_t      start, end;       /* inclusive; END_MAX: to the end, however far */
};

struct kflock {                     /* struct flock, x86-64 */
    int16_t l_type, l_whence;
    int32_t pad;
    int64_t l_start, l_len;
    int32_t l_pid, pad2;
};

static struct rlock *locks;
static int lock_chan;

static bool overlaps(const struct rlock *l, uint64_t s, uint64_t e) { return l->start <= e && s <= l->end; }

/* Remove owner's locks on vn within [s, e], keeping the parts outside it. */
static int cut(struct vnode *vn, uintptr_t owner, uint64_t s, uint64_t e)
{
    for (struct rlock **pp = &locks; *pp; ) {
        struct rlock *l = *pp;
        if (l->vn != vn || l->owner != owner || !overlaps(l, s, e)) { pp = &l->next; continue; }
        if (l->start < s && l->end > e) {               /* the middle goes: split in two */
            struct rlock *r = kmalloc(sizeof *r);
            if (!r) return -ENOLCK;
            *r = *l;
            r->start = e + 1;
            l->end = s - 1;
            r->next = l->next;
            l->next = r;
            return 0;
        }
        if (l->start < s) { l->end = s - 1; pp = &l->next; continue; }
        if (l->end > e) { l->start = e + 1; pp = &l->next; continue; }
        *pp = l->next;
        kfree(l);
    }
    return 0;
}

static struct rlock *conflict(struct vnode *vn, uintptr_t owner, int type, uint64_t s, uint64_t e)
{
    for (struct rlock *l = locks; l; l = l->next)
        if (l->vn == vn && l->owner != owner && overlaps(l, s, e) && (type == F_WRLCK || l->type == F_WRLCK))
            return l;
    return NULL;
}

/* The range a struct flock names, as [*s, *e]. */
static int range_of(struct file *f, const struct kflock *k, uint64_t *s, uint64_t *e)
{
    int64_t base;
    switch (k->l_whence) {
    case 0: base = 0; break;
    case 1: base = (int64_t)f->off; break;
    case 2: base = (int64_t)f->vn->size; break;
    default: return -EINVAL;
    }
    int64_t start = base + k->l_start;
    if (k->l_len > 0) {
        if (start < 0) return -EINVAL;
        *s = (uint64_t)start;
        *e = (uint64_t)(start + k->l_len - 1);
    } else if (k->l_len == 0) {
        if (start < 0) return -EINVAL;
        *s = (uint64_t)start;
        *e = END_MAX;
    } else {                                            /* the bytes before start */
        if (start + k->l_len < 0) return -EINVAL;
        *s = (uint64_t)(start + k->l_len);
        *e = (uint64_t)start - 1;
    }
    return 0;
}

static uintptr_t owner_of(struct file *f, bool ofd)
{
    return ofd ? (uintptr_t)f : POSIX_OWNER(current_task()->tgid);
}

int vfs_lock_get(struct file *f, void *kfl, bool ofd)
{
    struct kflock *k = kfl;
    uint64_t s, e;
    if (k->l_type != F_RDLCK && k->l_type != F_WRLCK) return -EINVAL;
    if (ofd && k->l_pid) return -EINVAL;
    int r = range_of(f, k, &s, &e);
    if (r) return r;
    uint64_t fl = irq_save();
    struct rlock *l = conflict(f->vn, owner_of(f, ofd), k->l_type, s, e);
    if (!l) {
        k->l_type = F_UNLCK;
    } else {
        k->l_type = (int16_t)l->type;
        k->l_whence = 0;
        k->l_start = (int64_t)l->start;
        k->l_len = l->end == END_MAX ? 0 : (int64_t)(l->end - l->start + 1);
        k->l_pid = l->pid;
    }
    irq_restore(fl);
    return 0;
}

int vfs_lock_set(struct file *f, void *kfl, bool ofd, bool wait)
{
    struct kflock *k = kfl;
    uint64_t s, e;
    if (k->l_type != F_RDLCK && k->l_type != F_WRLCK && k->l_type != F_UNLCK) return -EINVAL;
    if (ofd && k->l_pid) return -EINVAL;
    int acc = f->flags & O_ACCMODE;
    if (k->l_type == F_RDLCK && acc == O_WRONLY) return -EBADF;
    if (k->l_type == F_WRLCK && acc == O_RDONLY) return -EBADF;
    int r = range_of(f, k, &s, &e);
    if (r) return r;
    uintptr_t owner = owner_of(f, ofd);
    uint64_t fl = irq_save();
    if (k->l_type == F_UNLCK) {
        r = cut(f->vn, owner, s, e);
        wakeup(&lock_chan);
        irq_restore(fl);
        return r;
    }
    while (conflict(f->vn, owner, k->l_type, s, e)) {
        if (!wait) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        sleep_on(&lock_chan);
    }
    struct rlock *n = kmalloc(sizeof *n);
    if (!n || (r = cut(f->vn, owner, s, e)) < 0) { kfree(n); irq_restore(fl); return n ? r : -ENOLCK; }
    n->vn = f->vn;
    n->owner = owner;
    n->pid = ofd ? -1 : current_task()->tgid;
    n->type = k->l_type;
    n->start = s;
    n->end = e;
    n->next = locks;
    locks = n;
    if (k->l_type == F_RDLCK) wakeup(&lock_chan);       /* a write lock may have become a read lock */
    irq_restore(fl);
    return 0;
}

/* Drop the locks owned by `owner` (on vn, or on every file if vn is NULL). */
static void drop(struct vnode *vn, uintptr_t owner)
{
    if (!locks) return;
    uint64_t fl = irq_save();
    bool any = false;
    for (struct rlock **pp = &locks; *pp; ) {
        struct rlock *l = *pp;
        if (l->owner == owner && (!vn || l->vn == vn)) { *pp = l->next; kfree(l); any = true; }
        else pp = &l->next;
    }
    if (any) wakeup(&lock_chan);
    irq_restore(fl);
}

/* A descriptor of f is being closed by the current process. */
void vfs_locks_fd_closed(struct file *f)
{
    if (locks && f && f->vn) drop(f->vn, POSIX_OWNER(current_task()->tgid));
}

/* The open file itself is going away: its OFD locks. */
void vfs_locks_file_gone(struct file *f)
{
    if (locks) drop(NULL, (uintptr_t)f);
}
