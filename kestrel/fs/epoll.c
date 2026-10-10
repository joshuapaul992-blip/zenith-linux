/* fs/epoll.c -- epoll(7): readiness of many descriptors at once
 *
 * An epoll instance is an anonymous file holding a list of watches
 * (struct epitem): which file (and the descriptor it was added under), the
 * events wanted, and the user's 64-bit cookie. A watch does not keep its
 * file open: the last close of a file removes it from every set watching it
 * (epoll_file_closing, from vfs_close), as on Linux.
 *
 * epoll_wait() asks each watched file for its readiness (vfs_poll) and
 * sleeps on the poller sequence (task.h: poll_sleep) when nothing is ready,
 * so any state change anywhere wakes it to look again, and a file whose
 * readiness changes on its own (timerfd) bounds the sleep. Level-triggered
 * watches report while the condition holds. Edge-triggered ones (EPOLLET)
 * report readiness that is new since the last report; any I/O on the file
 * (or a readiness that went away and came back) re-arms them, so the usual
 * "read until EAGAIN, then wait" loop never misses data. EPOLLONESHOT
 * disables a watch after one report until EPOLL_CTL_MOD.
 *
 * The list is changed and scanned with interrupts off; epoll sets inside
 * epoll sets are allowed to a depth of 4, and loops are refused (ELOOP).
 * Reported watches move to the end of the list: with maxevents smaller
 * than the number of ready files, every file still gets its turn. */
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/string.h>

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDNORM    0x040
#define EPOLLRDBAND    0x080
#define EPOLLWRNORM    0x100
#define EPOLLWRBAND    0x200
#define EPOLLMSG       0x400
#define EPOLLRDHUP     0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)
#define EPOLL_EVENTS   (EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM | EPOLLRDBAND | \
                        EPOLLWRNORM | EPOLLWRBAND | EPOLLMSG | EPOLLRDHUP)
#define EPOLL_FLAGS    (EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET)

#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3
#define EPOLL_CLOEXEC  02000000

#define MAX_WATCHES    8192             /* per instance (max_user_watches) */
#define MAX_DEPTH      4

struct kepoll_event { uint32_t events; uint64_t data; } __attribute__((packed));

struct eventpoll;
struct epitem {
    struct eventpoll *ep;
    struct file   *file;                /* not a reference: see epoll_file_closing */
    int            fd;
    uint32_t       events;              /* EPOLL* wanted, with the flag bits */
    uint64_t       data;
    bool           armed;               /* EPOLLONESHOT: false once reported */
    uint32_t       et_seen;             /* EPOLLET: readiness already reported */
    struct epitem *next;                /* in ep->items */
    struct epitem *fnext;               /* in file->ep_links */
};

struct eventpoll {
    struct vnode   vn;                  /* vn.data == this */
    struct epitem *items;
    int            nitems;
};

static const struct vnode_ops epoll_ops;

bool epoll_is(struct file *f) { return f && f->vn && f->vn->ops == &epoll_ops; }
static struct eventpoll *ep_of(struct file *f) { return f->vn->data; }

int epoll_create_file(int flags, struct file **out)
{
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;
    struct eventpoll *ep = kzalloc(sizeof *ep);
    if (!ep) return -ENOMEM;
    strlcpy(ep->vn.name, "anon_inode:[eventpoll]", sizeof ep->vn.name);
    ep->vn.type = VCHR;
    ep->vn.mode = 0600;
    ep->vn.ops = &epoll_ops;
    ep->vn.data = ep;
    if (!(*out = vfs_file_new(&ep->vn, O_RDWR))) { kfree(ep); return -ENOMEM; }
    return 0;
}

static void unlink_from_file(struct epitem *it)
{
    for (struct epitem **pp = &it->file->ep_links; *pp; pp = &(*pp)->fnext)
        if (*pp == it) { *pp = it->fnext; return; }
}

static void unlink_from_ep(struct epitem *it)
{
    for (struct epitem **pp = &it->ep->items; *pp; pp = &(*pp)->next)
        if (*pp == it) { *pp = it->next; it->ep->nitems--; return; }
}

/* Is `target` (an epoll set) or anything nested in it the set `ep`? */
static bool reaches(struct eventpoll *from, struct eventpoll *ep, int depth)
{
    if (from == ep) return true;
    if (depth > MAX_DEPTH) return true;                 /* too deep counts as a loop */
    for (struct epitem *it = from->items; it; it = it->next)
        if (epoll_is(it->file) && reaches(ep_of(it->file), ep, depth + 1)) return true;
    return false;
}

static struct epitem *find(struct eventpoll *ep, struct file *f, int fd)
{
    for (struct epitem *it = ep->items; it; it = it->next)
        if (it->file == f && it->fd == fd) return it;
    return NULL;
}

int epoll_ctl_file(struct file *epf, int op, int fd, struct file *target, uint32_t events, uint64_t data)
{
    if (!epoll_is(epf)) return -EINVAL;
    struct eventpoll *ep = ep_of(epf);
    if (target == epf) return -EINVAL;
    if (op != EPOLL_CTL_DEL && (events & ~(uint32_t)(EPOLL_EVENTS | EPOLL_FLAGS))) return -EINVAL;
    if (!target->vn || !target->vn->ops || !target->vn->ops->poll) return -EPERM;   /* regular files */
    uint64_t fl = irq_save();
    struct epitem *it = find(ep, target, fd);
    int r = 0;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (it) { r = -EEXIST; break; }
        if (ep->nitems >= MAX_WATCHES) { r = -ENOSPC; break; }
        if (epoll_is(target) && reaches(ep_of(target), ep, 1)) { r = -ELOOP; break; }
        irq_restore(fl);
        it = kzalloc(sizeof *it);
        fl = irq_save();
        if (!it) { r = -ENOMEM; break; }
        if (find(ep, target, fd)) { kfree(it); r = -EEXIST; break; }    /* raced with another thread */
        it->ep = ep;
        it->file = target;
        it->fd = fd;
        it->events = events;
        it->data = data;
        it->armed = true;
        it->next = ep->items;                           /* new watches are looked at first */
        ep->items = it;
        ep->nitems++;
        it->fnext = target->ep_links;
        target->ep_links = it;
        break;
    case EPOLL_CTL_MOD:
        if (!it) { r = -ENOENT; break; }
        if ((events | it->events) & EPOLLEXCLUSIVE) { r = -EINVAL; break; }   /* as Linux */
        it->events = events;
        it->data = data;
        it->armed = true;
        it->et_seen = 0;
        break;
    case EPOLL_CTL_DEL:
        if (!it) { r = -ENOENT; break; }
        unlink_from_ep(it);
        unlink_from_file(it);
        kfree(it);
        break;
    default:
        r = -EINVAL;
    }
    irq_restore(fl);
    if (!r) wakeup(ep);                                 /* a waiter may have something to report now */
    return r;
}

/* The last descriptor of f is going: drop every watch on it. */
void epoll_file_closing(struct file *f)
{
    uint64_t fl = irq_save();
    while (f->ep_links) {
        struct epitem *it = f->ep_links;
        f->ep_links = it->fnext;
        unlink_from_ep(it);
        kfree(it);
    }
    irq_restore(fl);
}

void epoll_file_used(struct file *f)
{
    for (struct epitem *it = f->ep_links; it; it = it->fnext) it->et_seen = 0;
}

static int epoll_poll(struct file *f, int events);

/* Readiness of one watch, in EPOLL* bits (always including ERR and HUP). */
static uint32_t item_ready(struct epitem *it, int depth)
{
    uint32_t want = it->events;
    int r;
    if (epoll_is(it->file)) r = depth < MAX_DEPTH ? epoll_poll(it->file, POLLIN) : 0;
    else r = vfs_poll(it->file, POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP);
    uint32_t rev = (uint32_t)r & (EPOLLERR | EPOLLHUP);
    if (r & POLLIN) rev |= want & (EPOLLIN | EPOLLRDNORM);
    if (r & POLLOUT) rev |= want & (EPOLLOUT | EPOLLWRNORM);
    if (r & POLLPRI) rev |= want & (EPOLLPRI | EPOLLRDBAND);
    if ((r & POLLHUP) && (want & EPOLLRDHUP)) rev |= EPOLLRDHUP;
    return rev;
}

/* Collect up to max events (interrupts off). `peek`: only say whether any
 * would be reported, changing nothing. *deadline: the earliest time a
 * watched file's readiness changes by itself. */
static int scan(struct eventpoll *ep, struct kepoll_event *out, int max, bool peek, uint64_t *deadline, int depth)
{
    int n = 0;
    struct epitem *done = NULL, **done_tail = &done;
    for (struct epitem **pp = &ep->items; *pp && n < max;) {
        struct epitem *it = *pp;
        if (deadline) {
            uint64_t d = vfs_poll_deadline(it->file);
            if (d && (!*deadline || d < *deadline)) *deadline = d;
        }
        uint32_t rev = it->armed ? item_ready(it, depth) : 0;
        if (it->events & EPOLLET) {
            uint32_t fresh = rev & ~it->et_seen;
            if (!peek) it->et_seen = rev;               /* gone bits re-arm when they return */
            rev = fresh;
        }
        if (!rev) { pp = &it->next; continue; }
        if (peek) return 1;
        out[n].events = rev;
        out[n].data = it->data;
        n++;
        if (it->events & EPOLLONESHOT) it->armed = false;
        *pp = it->next;                                 /* to the end: fairness */
        it->next = NULL;
        *done_tail = it;
        done_tail = &it->next;
    }
    if (done) {
        struct epitem **pp = &ep->items;
        while (*pp) pp = &(*pp)->next;
        *pp = done;
    }
    return n;
}

int epoll_wait_file(struct file *epf, void *kevents, int maxevents, int64_t timeout_ms)
{
    if (!epoll_is(epf)) return -EINVAL;
    struct eventpoll *ep = ep_of(epf);
    uint64_t deadline = timeout_ms > 0 ? time_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        uint64_t seq = poll_seq_read(), tdl = 0;
        uint64_t fl = irq_save();
        int n = scan(ep, kevents, maxevents, false, &tdl, 0);
        irq_restore(fl);
        if (n || timeout_ms == 0) return n;
        if (signal_pending()) return -EINTR;
        if (timeout_ms > 0 && time_ms() >= deadline) return 0;
        uint64_t d = timeout_ms > 0 ? deadline : 0;
        if (tdl && (!d || tdl < d)) d = tdl;
        poll_sleep(seq, d);
    }
}

/* An epoll fd is readable when epoll_wait would report something. */
static int epoll_poll(struct file *f, int events)
{
    static int depth;                                   /* nesting (interrupts are off meanwhile) */
    uint64_t fl = irq_save();
    depth++;
    int r = scan(ep_of(f), NULL, 1, true, NULL, depth) ? POLLIN : 0;
    depth--;
    irq_restore(fl);
    return r & events;
}

static uint64_t epoll_deadline(struct file *f)
{
    uint64_t d = 0;
    uint64_t fl = irq_save();
    for (struct epitem *it = ep_of(f)->items; it; it = it->next) {
        if (epoll_is(it->file)) continue;               /* one level is enough for timers */
        uint64_t x = vfs_poll_deadline(it->file);
        if (x && (!d || x < d)) d = x;
    }
    irq_restore(fl);
    return d;
}

static void epoll_release(struct file *f)
{
    struct eventpoll *ep = ep_of(f);
    uint64_t fl = irq_save();
    while (ep->items) {
        struct epitem *it = ep->items;
        ep->items = it->next;
        unlink_from_file(it);
        kfree(it);
    }
    irq_restore(fl);
    kfree(ep);
}

static const struct vnode_ops epoll_ops = {
    .poll = epoll_poll, .release = epoll_release, .poll_deadline = epoll_deadline,
};
