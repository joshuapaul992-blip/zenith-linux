/* fs/inotify.c -- inotify(7): file system events for watched files and directories
 *
 * An inotify descriptor holds watches (a vnode and the events wanted) and a
 * queue of struct inotify_event records, read whole. The VFS reports what
 * happens through inotify_event(): on a watched directory for the entries
 * in it (with the entry's name), and on a watched file or directory itself
 * (no name). Watches end when their vnode is freed (IN_IGNORED). The same
 * event twice in a row is queued once, as Linux does; a full queue gets one
 * IN_Q_OVERFLOW. GLib's file monitors (GTK, Firefox) use this. */
#include <kernel/vfs.h>
#include <kernel/inotify.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>

#define NONBLOCK_FLAG   04000
#define CLOEXEC_FLAG    02000000
#define QUEUE_MAX       16384           /* bytes of events per descriptor */
#define WATCHES_MAX     8192            /* in all (/proc/sys/fs/inotify/max_user_watches) */
#define FIONREAD        0x541B

struct watch {
    struct watch *next;
    struct vnode *vn;
    int           wd;
    uint32_t      mask;
};

struct ino_ev {                         /* a queued event; name follows */
    struct ino_ev *next;
    int32_t  wd;
    uint32_t mask, cookie, len;         /* len: name bytes with padding, as read */
    char     name[];
};

struct inotify {
    struct vnode    vn;
    struct inotify *next;               /* every instance */
    struct watch   *watches;
    int             next_wd;
    struct ino_ev  *head, *tail;
    size_t          bytes;
    bool            overflowed;
};

static struct inotify *instances;
static int nwatches;

static size_t ev_size(const struct ino_ev *e) { return 16 + e->len; }

static void queue(struct inotify *in, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    size_t nl = name ? strlen(name) : 0;
    uint32_t len = nl ? (uint32_t)((nl + 1 + 15) & ~(size_t)15) : 0;    /* NUL, padded to 16 */
    struct ino_ev *t = in->tail;
    if (t && t->wd == wd && t->mask == mask && t->cookie == cookie &&       /* the same again: once */
        (nl ? (t->len && !strcmp(t->name, name)) : !t->len))
        return;
    if (in->bytes + 16 + len > QUEUE_MAX) {
        if (in->overflowed) return;
        in->overflowed = true;
        wd = -1; mask = IN_Q_OVERFLOW; cookie = 0; len = 0; nl = 0;
    }
    struct ino_ev *e = kzalloc(sizeof *e + len);
    if (!e) return;
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    e->len = len;
    if (nl) memcpy(e->name, name, nl);
    if (in->tail) in->tail->next = e; else in->head = e;
    in->tail = e;
    in->bytes += ev_size(e);
    wakeup(in);
}

/* Remove a watch from its instance; IN_IGNORED tells the reader. */
static void drop_watch(struct inotify *in, struct watch **pp)
{
    struct watch *w = *pp;
    *pp = w->next;
    queue(in, w->wd, IN_IGNORED, 0, NULL);
    kfree(w);
    nwatches--;
}

void inotify_event(struct vnode *vn, uint32_t mask, const char *name, uint32_t cookie)
{
    if (!nwatches || !vn) return;
    uint64_t fl = irq_save();
    for (struct inotify *in = instances; in; in = in->next) {
        for (struct watch **pp = &in->watches; *pp; ) {
            struct watch *w = *pp;
            uint32_t m = mask & ~(uint32_t)IN_ISDIR;
            if (w->vn != vn || !(w->mask & m)) { pp = &w->next; continue; }
            queue(in, w->wd, mask, cookie, name);
            if (w->mask & IN_ONESHOT) { drop_watch(in, pp); continue; }
            pp = &w->next;
        }
    }
    irq_restore(fl);
}

/* Something happened to vn itself: its own watches, and its directory's
 * (with its name). */
void inotify_self(struct vnode *vn, uint32_t mask)
{
    if (!nwatches || !vn) return;
    uint32_t dir = vn->type == VDIR ? IN_ISDIR : 0;
    inotify_event(vn, mask | dir, NULL, 0);
    if (vn->parent && vn->parent != vn) inotify_event(vn->parent, mask | dir, vn->name, 0);
}

void inotify_vnode_gone(struct vnode *vn)
{
    if (!nwatches) return;
    uint64_t fl = irq_save();
    for (struct inotify *in = instances; in; in = in->next)
        for (struct watch **pp = &in->watches; *pp; ) {
            if ((*pp)->vn == vn) drop_watch(in, pp);
            else pp = &(*pp)->next;
        }
    irq_restore(fl);
}

uint32_t inotify_cookie(void)
{
    static uint32_t c;
    return ++c ? c : ++c;
}

/* ---- the descriptor ------------------------------------------------------------- */
static ssize_t in_read(struct file *f, void *buf, size_t len)
{
    struct inotify *in = f->vn->data;
    uint64_t fl = irq_save();
    while (!in->head) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(in);
    }
    if (ev_size(in->head) > len) { irq_restore(fl); return -EINVAL; }   /* not even one fits */
    size_t n = 0;
    uint8_t *out = buf;
    while (in->head && n + ev_size(in->head) <= len) {
        struct ino_ev *e = in->head;
        memcpy(out + n, &e->wd, 16);                    /* wd, mask, cookie, len */
        memcpy(out + n + 16, e->name, e->len);
        n += ev_size(e);
        in->head = e->next;
        if (!in->head) in->tail = NULL;
        in->bytes -= ev_size(e);
        kfree(e);
    }
    if (!in->head) in->overflowed = false;
    irq_restore(fl);
    return (ssize_t)n;
}

static int in_poll(struct file *f, int events)
{
    struct inotify *in = f->vn->data;
    return in->head ? (events & (POLLIN | POLLRDNORM)) : 0;
}

static int in_ioctl(struct file *f, unsigned long req, void *arg)
{
    struct inotify *in = f->vn->data;
    if (req != FIONREAD) return -ENOTTY;
    *(int *)arg = (int)in->bytes;                       /* a kernel copy (sys_ioctl) */
    return 0;
}

static void in_release(struct file *f)
{
    struct inotify *in = f->vn->data;
    uint64_t fl = irq_save();
    for (struct inotify **pp = &instances; *pp; pp = &(*pp)->next)
        if (*pp == in) { *pp = in->next; break; }
    while (in->watches) { struct watch *w = in->watches; in->watches = w->next; kfree(w); nwatches--; }
    while (in->head) { struct ino_ev *e = in->head; in->head = e->next; kfree(e); }
    irq_restore(fl);
    kfree(in);
}

static const struct vnode_ops in_ops = {
    .fread = in_read, .poll = in_poll, .fioctl = in_ioctl, .release = in_release,
};

int inotify_create(int flags, struct file **out)
{
    if (flags & ~(NONBLOCK_FLAG | CLOEXEC_FLAG)) return -EINVAL;
    struct inotify *in = kzalloc(sizeof *in);
    if (!in) return -ENOMEM;
    strlcpy(in->vn.name, "anon_inode:inotify", sizeof in->vn.name);
    in->vn.type = VCHR;
    in->vn.mode = 0600;
    in->vn.ops = &in_ops;
    in->vn.data = in;
    in->next_wd = 1;
    *out = vfs_file_new(&in->vn, O_RDONLY | (flags & NONBLOCK_FLAG ? O_NONBLOCK : 0));
    if (!*out) { kfree(in); return -ENOMEM; }
    uint64_t fl = irq_save();
    in->next = instances;
    instances = in;
    irq_restore(fl);
    return 0;
}

bool inotify_is(struct file *f) { return f && f->vn && f->vn->ops == &in_ops; }

int inotify_add(struct file *f, struct vnode *vn, uint32_t mask)
{
    if (!inotify_is(f)) return -EINVAL;
    struct inotify *in = f->vn->data;
    if ((mask & IN_ONLYDIR) && vn->type != VDIR) return -ENOTDIR;
    uint32_t events = mask & IN_ALL_EVENTS;
    if (!events) return -EINVAL;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -EINVAL;
    uint64_t fl = irq_save();
    for (struct watch *w = in->watches; w; w = w->next)
        if (w->vn == vn) {
            if (mask & IN_MASK_CREATE) { irq_restore(fl); return -EEXIST; }
            w->mask = (mask & IN_MASK_ADD) ? w->mask | events : events;
            w->mask |= mask & IN_ONESHOT;
            irq_restore(fl);
            return w->wd;
        }
    if (nwatches >= WATCHES_MAX) { irq_restore(fl); return -ENOSPC; }
    struct watch *w = kzalloc(sizeof *w);
    if (!w) { irq_restore(fl); return -ENOMEM; }
    w->vn = vn;
    w->wd = in->next_wd++;
    w->mask = events | (mask & IN_ONESHOT);
    w->next = in->watches;
    in->watches = w;
    nwatches++;
    irq_restore(fl);
    return w->wd;
}

int inotify_rm(struct file *f, int wd)
{
    if (!inotify_is(f)) return -EINVAL;
    struct inotify *in = f->vn->data;
    uint64_t fl = irq_save();
    for (struct watch **pp = &in->watches; *pp; pp = &(*pp)->next)
        if ((*pp)->wd == wd) { drop_watch(in, pp); irq_restore(fl); return 0; }
    irq_restore(fl);
    return -EINVAL;
}
