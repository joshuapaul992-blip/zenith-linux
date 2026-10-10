/* fs/anonfd.c -- event, timer and signal descriptors
 *
 *   eventfd(2)    a 64-bit counter: write adds, read takes it (or 1 in
 *                 EFD_SEMAPHORE mode); readable while non-zero
 *   timerfd(2)    a one-shot or periodic timer: read returns how many times
 *                 it expired since the last read; readable after expiry
 *   signalfd(2)   pending signals of the reading thread in a mask, read as
 *                 struct signalfd_siginfo; readable while one is pending
 *
 * Each is an anonymous vnode embedded in its object, freed at the last
 * close. read() and write() get kernel buffers (syscall.c bounces user
 * memory), always whole 8-byte or 128-byte units. Timers keep nanoseconds
 * on the monotonic clock (time_ns); a sleeping reader, poll() and epoll
 * wake at the expiry through sleep_on_until and the poll_deadline op. */
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/string.h>

#define EFD_SEMAPHORE   1
#define NONBLOCK_FLAG   04000           /* EFD_/TFD_/SFD_NONBLOCK = O_NONBLOCK */
#define CLOEXEC_FLAG    02000000
#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7
#define NS              1000000000ull

static struct vnode *anon_vnode(struct vnode *vn, const char *name, const struct vnode_ops *ops, void *data)
{
    strlcpy(vn->name, name, sizeof vn->name);
    vn->type = VCHR;
    vn->mode = 0600;
    vn->ops = ops;
    vn->data = data;
    return vn;
}

static int new_file(struct vnode *vn, int flags, struct file **out)
{
    *out = vfs_file_new(vn, O_RDWR | (flags & NONBLOCK_FLAG ? O_NONBLOCK : 0));
    return *out ? 0 : -ENOMEM;
}

static void free_object(struct file *f) { kfree(f->vn->data); }

/* ---- eventfd ------------------------------------------------------------------- */
struct eventfd {
    struct vnode vn;
    uint64_t     count;
    bool         semaphore;
};

#define EFD_MAX 0xfffffffffffffffeull

static ssize_t efd_read(struct file *f, void *buf, size_t len)
{
    struct eventfd *e = f->vn->data;
    if (len < 8) return -EINVAL;
    uint64_t fl = irq_save();
    while (!e->count) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(e);
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    wakeup(e);                                          /* writers waiting for room */
    irq_restore(fl);
    memcpy(buf, &v, 8);
    return 8;
}

static ssize_t efd_write(struct file *f, const void *buf, size_t len)
{
    struct eventfd *e = f->vn->data;
    if (len < 8) return -EINVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == ~0ull) return -EINVAL;
    uint64_t fl = irq_save();
    while (EFD_MAX - e->count < v) {                    /* would overflow: wait for a read */
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(e);
    }
    e->count += v;
    if (v) wakeup(e);
    irq_restore(fl);
    return 8;
}

static int efd_poll(struct file *f, int events)
{
    struct eventfd *e = f->vn->data;
    int r = 0;
    if (e->count) r |= POLLIN;
    if (e->count < EFD_MAX) r |= POLLOUT;
    return r & events;
}

static const struct vnode_ops efd_ops = {
    .fread = efd_read, .fwrite = efd_write, .poll = efd_poll, .release = free_object,
};

int eventfd_create(uint32_t initval, int flags, struct file **out)
{
    if (flags & ~(EFD_SEMAPHORE | NONBLOCK_FLAG | CLOEXEC_FLAG)) return -EINVAL;
    struct eventfd *e = kzalloc(sizeof *e);
    if (!e) return -ENOMEM;
    e->count = initval;
    e->semaphore = flags & EFD_SEMAPHORE;
    int r = new_file(anon_vnode(&e->vn, "anon_inode:[eventfd]", &efd_ops, e), flags, out);
    if (r) kfree(e);
    return r;
}

/* ---- timerfd -------------------------------------------------------------------- */
struct timerfd {
    struct vnode vn;
    int          clock;
    uint64_t     next;                  /* time_ns() of the next expiry; 0 = disarmed */
    uint64_t     interval;              /* ns; 0 = one-shot */
    uint64_t     expired;               /* expirations not read yet */
};

/* Count expirations up to now (interrupts off). */
static void tfd_update(struct timerfd *t)
{
    if (!t->next) return;
    uint64_t now = time_ns();
    if (now < t->next) return;
    if (t->interval) {
        uint64_t k = (now - t->next) / t->interval + 1;
        t->expired += k;
        t->next += k * t->interval;
    } else {
        t->expired++;
        t->next = 0;
    }
}

static uint64_t ns_to_deadline_ms(uint64_t ns) { return (ns + 999999) / 1000000; }

static ssize_t tfd_read(struct file *f, void *buf, size_t len)
{
    struct timerfd *t = f->vn->data;
    if (len < 8) return -EINVAL;
    uint64_t fl = irq_save();
    for (;;) {
        tfd_update(t);
        if (t->expired) break;
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on_until(t, t->next ? ns_to_deadline_ms(t->next) : 0);    /* settime wakes us too */
    }
    uint64_t v = t->expired;
    t->expired = 0;
    irq_restore(fl);
    memcpy(buf, &v, 8);
    return 8;
}

static int tfd_poll(struct file *f, int events)
{
    struct timerfd *t = f->vn->data;
    uint64_t fl = irq_save();
    tfd_update(t);
    int r = t->expired ? POLLIN : 0;
    irq_restore(fl);
    return r & events;
}

static uint64_t tfd_deadline(struct file *f)
{
    struct timerfd *t = f->vn->data;
    return t->next ? ns_to_deadline_ms(t->next) : 0;
}

static const struct vnode_ops tfd_ops = {
    .fread = tfd_read, .poll = tfd_poll, .poll_deadline = tfd_deadline, .release = free_object,
};

static bool is_timerfd(struct file *f) { return f && f->vn && f->vn->ops == &tfd_ops; }

int timerfd_create_file(int clockid, int flags, struct file **out)
{
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME) return -EINVAL;
    if (flags & ~(NONBLOCK_FLAG | CLOEXEC_FLAG)) return -EINVAL;
    struct timerfd *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    t->clock = clockid;
    int r = new_file(anon_vnode(&t->vn, "anon_inode:[timerfd]", &tfd_ops, t), flags, out);
    if (r) kfree(t);
    return r;
}

static void cur_value(struct timerfd *t, int64_t v[4])
{
    tfd_update(t);
    uint64_t left = t->next ? t->next - time_ns() : 0;
    if (t->next && (int64_t)left <= 0) left = 1;
    v[0] = (int64_t)(t->interval / NS); v[1] = (int64_t)(t->interval % NS);
    v[2] = (int64_t)(left / NS);        v[3] = (int64_t)(left % NS);
}

static bool ts_ok(int64_t s, int64_t ns) { return s >= 0 && ns >= 0 && ns < (int64_t)NS; }
static uint64_t ts_ns(int64_t s, int64_t ns)
{
    return (uint64_t)s > (1ull << 33) ? (1ull << 62) : (uint64_t)s * NS + (uint64_t)ns;   /* far future */
}

/* nv/old: { it_interval.tv_sec, .tv_nsec, it_value.tv_sec, .tv_nsec } */
int timerfd_settime_file(struct file *f, int flags, const int64_t nv[4], int64_t old[4])
{
    if (!is_timerfd(f)) return -EINVAL;
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) return -EINVAL;
    if (!ts_ok(nv[0], nv[1]) || !ts_ok(nv[2], nv[3])) return -EINVAL;
    struct timerfd *t = f->vn->data;
    uint64_t value = ts_ns(nv[2], nv[3]), now = time_ns();
    uint64_t fl = irq_save();
    if (old) cur_value(t, old);
    t->interval = ts_ns(nv[0], nv[1]);
    t->expired = 0;
    if (!value) t->next = 0;                            /* disarm */
    else if (!(flags & TFD_TIMER_ABSTIME)) t->next = now + value;
    else if (t->clock == CLOCK_REALTIME) {              /* wall time -> the monotonic clock */
        int64_t s, ns;
        time_realtime(&s, &ns);
        uint64_t rt = (uint64_t)s * NS + (uint64_t)ns;
        t->next = value > rt ? now + (value - rt) : now;
    } else {
        t->next = value;                                /* CLOCK_MONOTONIC = time since boot */
    }
    if (t->next == 0 && value) t->next = 1;
    wakeup(t);
    irq_restore(fl);
    return 0;
}

int timerfd_gettime_file(struct file *f, int64_t cur[4])
{
    if (!is_timerfd(f)) return -EINVAL;
    uint64_t fl = irq_save();
    cur_value(f->vn->data, cur);
    irq_restore(fl);
    return 0;
}

/* ---- signalfd ---------------------------------------------------------------------- */
struct sigfd {
    struct vnode vn;
    uint64_t     mask;
};

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno, ssi_code;
    uint32_t ssi_pid, ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid, ssi_band, ssi_overrun, ssi_trapno;
    int32_t  ssi_status, ssi_int;
    uint64_t ssi_ptr, ssi_utime, ssi_stime, ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t  pad[46];
};
_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd_siginfo layout");

static ssize_t sfd_read(struct file *f, void *buf, size_t len)
{
    struct sigfd *s = f->vn->data;
    size_t n = len / sizeof(struct signalfd_siginfo), got = 0;
    if (!n) return -EINVAL;
    uint64_t fl = irq_save();
    while (got < n) {
        struct ksig_info info;
        int sig = signal_dequeue(s->mask, &info);
        if (!sig) {
            if (got) break;
            if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
            if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
            sleep_on(&signal_wait_chan);
            continue;
        }
        struct signalfd_siginfo si;
        memset(&si, 0, sizeof si);
        si.ssi_signo = (uint32_t)sig;
        si.ssi_code = info.code;
        si.ssi_pid = (uint32_t)info.pid;
        si.ssi_uid = info.uid;
        si.ssi_status = info.status;
        si.ssi_trapno = info.trapno;
        si.ssi_addr = info.addr;
        memcpy((uint8_t *)buf + got * sizeof si, &si, sizeof si);
        got++;
    }
    irq_restore(fl);
    return (ssize_t)(got * sizeof(struct signalfd_siginfo));
}

static int sfd_poll(struct file *f, int events)
{
    struct sigfd *s = f->vn->data;
    return (signal_pending_mask() & s->mask) ? (POLLIN & events) : 0;
}

static const struct vnode_ops sfd_ops = { .fread = sfd_read, .poll = sfd_poll, .release = free_object };

/* signalfd4: a new descriptor (f == NULL), or a new mask for an existing one. */
int signalfd_file(struct file *f, uint64_t mask, int flags, struct file **out)
{
    if (flags & ~(NONBLOCK_FLAG | CLOEXEC_FLAG)) return -EINVAL;
    mask &= ~((1ull << 8) | (1ull << 18));              /* SIGKILL and SIGSTOP cannot be read */
    if (f) {
        if (!f->vn || f->vn->ops != &sfd_ops) return -EINVAL;
        ((struct sigfd *)f->vn->data)->mask = mask;
        *out = NULL;
        return 0;
    }
    struct sigfd *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    s->mask = mask;
    int r = new_file(anon_vnode(&s->vn, "anon_inode:[signalfd]", &sfd_ops, s), flags, out);
    if (r) kfree(s);
    return r;
}
