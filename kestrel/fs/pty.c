/* fs/pty.c -- pseudo-terminals: /dev/ptmx and /dev/pts/N
 *
 * Linux-compatible Unix98 ptys, enough for terminal emulators (xterm) and
 * shells. Opening /dev/ptmx allocates a pair and returns its master; the
 * slave appears as /dev/pts/N once unlocked (TIOCSPTLCK 0; TIOCGPTN gives N).
 *
 *   master write -> input processing (ICRNL, ISIG, canonical editing, echo)
 *                -> input queue  -> slave read
 *   slave write  -> output processing (OPOST/ONLCR) -> output queue
 *                -> master read; echo goes to the output queue as well
 *
 * Canonical mode keeps the line being edited apart and moves it to the
 * input queue on newline/EOL/EOF; a queue of line lengths lets a canonical
 * read return one line at a time (an EOF on an empty line reads as 0).
 * Raw mode honours VMIN/VTIME. The controlling terminal of a session is set
 * with TIOCSCTTY (or by a session leader opening a slave), and then gets
 * the keyboard signals (VINTR, VQUIT, VSUSP) for its foreground process
 * group, SIGWINCH on TIOCSWINSZ, and SIGHUP when the master closes.
 *
 * Locking: single CPU, interrupts off around queue updates. */
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/termios.h>
#include <kernel/posix.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/uvm.h>
#include <kernel/klog.h>
#include <kernel/string.h>

#define PTY_MAX     16
#define IN_BUF      4096            /* like N_TTY's read buffer               */
#define OUT_BUF     16384
#define LINE_MAX_   1024
#define LINEQ       64

struct pty {
    bool     used, locked;
    int      index;
    int      masters, slaves;       /* open files on each side                */
    bool     slave_seen;            /* a slave was opened (master reads EIO after) */
    struct ktermios tio;
    struct kwinsize ws;
    int      session, fg_pgrp;      /* controlling tty of this session        */

    uint8_t  in[IN_BUF];            /* ready for the slave to read            */
    size_t   in_head, in_count;
    uint16_t lineq[LINEQ];          /* canonical: lengths of complete lines   */
    int      lq_head, lq_count;
    char     line[LINE_MAX_];       /* canonical: line being edited          */
    size_t   line_len;

    uint8_t  out[OUT_BUF];          /* for the master to read                 */
    size_t   out_head, out_count;

    struct vnode master_vn;
};

static struct pty ptys[PTY_MAX];
static struct vnode *pts_nodes[PTY_MAX];
static const struct vnode_ops master_ops, slave_ops;

static bool canon(const struct pty *p) { return p->tio.c_lflag & TL_ICANON; }

/* ---- queues ------------------------------------------------------------- */
static void out_put(struct pty *p, uint8_t c)
{
    if (p->out_count == OUT_BUF) return;               /* full: drop (echo only) */
    p->out[(p->out_head + p->out_count++) % OUT_BUF] = c;
}

/* Output processing for a byte written by the slave (or echoed). */
static void out_char(struct pty *p, uint8_t c)
{
    if (p->tio.c_oflag & TO_OPOST) {
        if (c == '\n' && (p->tio.c_oflag & TO_ONLCR)) { out_put(p, '\r'); out_put(p, '\n'); return; }
        if (c == '\r' && (p->tio.c_oflag & TO_OCRNL)) c = '\n';
    }
    out_put(p, c);
}

static void echo_char(struct pty *p, uint8_t c)
{
    if ((p->tio.c_lflag & TL_ECHOCTL) && (c < 32 || c == 127) && c != '\n' && c != '\t') {
        out_put(p, '^');
        out_put(p, c == 127 ? '?' : (uint8_t)(c + 64));
    } else {
        out_char(p, c);
    }
}

static void in_put(struct pty *p, uint8_t c)
{
    if (p->in_count == IN_BUF) return;                  /* full: drop, like N_TTY */
    p->in[(p->in_head + p->in_count++) % IN_BUF] = c;
}

static void commit_line(struct pty *p)
{
    if (p->lq_count == LINEQ || p->in_count + p->line_len > IN_BUF) { p->line_len = 0; return; }
    for (size_t i = 0; i < p->line_len; i++) in_put(p, (uint8_t)p->line[i]);
    p->lineq[(p->lq_head + p->lq_count++) % LINEQ] = (uint16_t)p->line_len;
    p->line_len = 0;
}

static void flush_input(struct pty *p)
{
    p->in_head = p->in_count = 0;
    p->lq_head = p->lq_count = 0;
    p->line_len = 0;
}

static void erase_one(struct pty *p)
{
    if (!p->line_len) return;
    uint8_t c = (uint8_t)p->line[--p->line_len];
    if ((p->tio.c_lflag & TL_ECHO) && (p->tio.c_lflag & TL_ECHOE)) {
        int w = ((p->tio.c_lflag & TL_ECHOCTL) && (c < 32 || c == 127) && c != '\t') ? 2 : 1;
        while (w--) { out_put(p, '\b'); out_put(p, ' '); out_put(p, '\b'); }
    }
}

static void group_signal(struct pty *p, int sig)
{
    if (!p->fg_pgrp) return;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (t->state != TASK_UNUSED && t->state != TASK_ZOMBIE && t->user && t->pgid == p->fg_pgrp)
            signal_send(t, sig);
    }
}

/* Input processing of one byte typed into the master. */
static void input_char(struct pty *p, uint8_t c)
{
    const struct ktermios *t = &p->tio;
    if (t->c_iflag & TI_ISTRIP) c &= 0x7f;
    if (c == '\r') {
        if (t->c_iflag & TI_IGNCR) return;
        if (t->c_iflag & TI_ICRNL) c = '\n';
    } else if (c == '\n' && (t->c_iflag & TI_INLCR)) {
        c = '\r';
    }
    if (t->c_lflag & TL_ISIG) {
        int sig = c == t->c_cc[VINTR] ? SIGINT : c == t->c_cc[VQUIT] ? SIGQUIT : c == t->c_cc[VSUSP] ? SIGTSTP : 0;
        if (sig && c) {
            if (!(t->c_lflag & TL_NOFLSH)) flush_input(p);
            if (t->c_lflag & TL_ECHO) echo_char(p, c);
            group_signal(p, sig);
            return;
        }
    }
    if (canon(p)) {
        if (c == t->c_cc[VERASE] && c) { erase_one(p); return; }
        if ((t->c_lflag & TL_IEXTEN) && c == t->c_cc[VWERASE] && c) {
            while (p->line_len && p->line[p->line_len - 1] == ' ') erase_one(p);
            while (p->line_len && p->line[p->line_len - 1] != ' ') erase_one(p);
            return;
        }
        if (c == t->c_cc[VKILL] && c) { while (p->line_len) erase_one(p); return; }
        if (c == t->c_cc[VEOF] && c) { commit_line(p); return; }
        bool eol = c == '\n' || (c && (c == t->c_cc[VEOL] || c == t->c_cc[VEOL2]));
        if (p->line_len < LINE_MAX_ - 1 || eol) p->line[p->line_len++] = (char)c;
        if ((t->c_lflag & TL_ECHO) || (c == '\n' && (t->c_lflag & TL_ECHONL))) echo_char(p, c);
        if (eol) commit_line(p);
        return;
    }
    in_put(p, c);
    if (t->c_lflag & TL_ECHO) echo_char(p, c);
}

/* Switching ICANON: the half-edited line and the line structure follow. */
static void set_termios(struct pty *p, const struct ktermios *n)
{
    bool was = canon(p);
    p->tio = *n;
    if (was && !canon(p)) {
        for (size_t i = 0; i < p->line_len; i++) in_put(p, (uint8_t)p->line[i]);
        p->line_len = 0;
        p->lq_head = p->lq_count = 0;
    } else if (!was && canon(p) && p->in_count) {
        p->lineq[0] = (uint16_t)p->in_count;            /* what is queued reads as one line */
        p->lq_head = 0;
        p->lq_count = 1;
    }
}

static size_t slave_readable(const struct pty *p)
{
    if (canon(p)) return p->lq_count ? (size_t)p->lineq[p->lq_head] + 1 : 0;  /* +1: an empty EOF line counts */
    return p->in_count;
}

/* ---- allocation ---------------------------------------------------------- */
static void pty_defaults(struct pty *p)
{
    memset(&p->tio, 0, sizeof p->tio);
    p->tio.c_iflag = TI_ICRNL | TI_IXON | TI_IUTF8;
    p->tio.c_oflag = TO_OPOST | TO_ONLCR;
    p->tio.c_cflag = TC_B38400 | TC_CS8 | TC_CREAD | TC_HUPCL;
    p->tio.c_lflag = TL_ISIG | TL_ICANON | TL_ECHO | TL_ECHOE | TL_ECHOK | TL_ECHOCTL | TL_ECHOKE | TL_IEXTEN;
    static const uint8_t cc[KNCCS] = {
        [VINTR] = 3, [VQUIT] = 034, [VERASE] = 0177, [VKILL] = 025, [VEOF] = 4, [VTIME] = 0, [VMIN] = 1,
        [VSTART] = 021, [VSTOP] = 023, [VSUSP] = 032, [VREPRINT] = 022, [VDISCARD] = 017,
        [VWERASE] = 027, [VLNEXT] = 026,
    };
    memcpy(p->tio.c_cc, cc, sizeof cc);
    p->ws.ws_row = 24;
    p->ws.ws_col = 80;
}

static void pty_free_if_unused(struct pty *p)
{
    if (p->used && !p->masters && !p->slaves) {
        p->used = false;
        for (int i = 0; i < MAX_TASKS; i++) {           /* nobody's controlling tty any more */
            struct tcb *t = task_slot(i);
            if (t->state != TASK_UNUSED && t->ctty == p->index + 1) t->ctty = 0;
        }
    }
}

static int ptmx_open(struct vnode *vn, int flags, struct file **out)
{
    (void)vn;
    uint64_t fl = irq_save();
    struct pty *p = NULL;
    for (int i = 0; i < PTY_MAX; i++) if (!ptys[i].used) { p = &ptys[i]; break; }
    if (!p) { irq_restore(fl); return -ENOSPC; }
    int idx = (int)(p - ptys);
    memset(p, 0, __builtin_offsetof(struct pty, master_vn));
    p->used = true;
    p->locked = true;
    p->index = idx;
    pty_defaults(p);
    irq_restore(fl);

    struct vnode *m = &p->master_vn;
    memset(m, 0, sizeof *m);
    strlcpy(m->name, "ptmx", sizeof m->name);
    m->type = VCHR;
    m->mode = S_IFCHR | 0666;
    m->rdev = (5u << 8) | 2;
    m->ops = &master_ops;
    m->data = p;
    m->fs = vn->fs;
    struct file *f = vfs_file_new(m, flags);
    if (!f) { p->used = false; return -ENOMEM; }
    p->masters = 1;
    *out = f;
    return 0;
}

/* The calling process takes `p` as its session's controlling terminal. */
static void acquire_ctty(struct pty *p)
{
    struct tcb *t = current_task();
    p->session = t->sid;
    p->fg_pgrp = t->pgid;
    t->ctty = p->index + 1;
}

static int slave_open(struct vnode *vn, int flags, struct file **out)
{
    int idx = (int)(intptr_t)vn->ctx;
    struct pty *p = &ptys[idx];
    if (!p->used || p->locked || !p->masters) return -EIO;
    struct file *f = vfs_file_new(vn, flags);
    if (!f) return -ENOMEM;
    p->slaves++;
    p->slave_seen = true;
    struct tcb *t = current_task();
    if (!(flags & O_NOCTTY) && t->user && t->sid == t->pid && !t->ctty && !p->session) acquire_ctty(p);
    *out = f;
    return 0;
}

static struct pty *pty_of(struct file *f)
{
    if (f->vn->ops == &master_ops) return f->vn->data;
    return &ptys[(int)(intptr_t)f->vn->ctx];
}

/* ---- master side ----------------------------------------------------------- */
static ssize_t master_read(struct file *f, void *buf, size_t len)
{
    struct pty *p = pty_of(f);
    if (!len) return 0;
    uint64_t fl = irq_save();
    while (!p->out_count) {
        if (p->slave_seen && !p->slaves) { irq_restore(fl); return -EIO; }   /* slave side closed */
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(&p->out);
    }
    size_t n = len < p->out_count ? len : p->out_count;
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = p->out[(p->out_head + i) % OUT_BUF];
    p->out_head = (p->out_head + n) % OUT_BUF;
    p->out_count -= n;
    wakeup(&p->out);                                    /* slave writers waiting for room */
    irq_restore(fl);
    return (ssize_t)n;
}

static ssize_t master_write(struct file *f, const void *buf, size_t len)
{
    struct pty *p = pty_of(f);
    uint64_t fl = irq_save();
    size_t done = 0;
    while (done < len) {
        if (!canon(p) && p->in_count >= IN_BUF) {      /* canonical input is dropped when full */
            if (f->flags & O_NONBLOCK) break;
            if (signal_pending()) { irq_restore(fl); return done ? (ssize_t)done : -ERESTARTSYS; }
            sleep_on(&p->in);
            continue;
        }
        input_char(p, ((const uint8_t *)buf)[done++]);
    }
    wakeup(&p->in);                                     /* slave readers */
    wakeup(&p->out);                                    /* echo for master readers */
    irq_restore(fl);
    return done ? (ssize_t)done : -EAGAIN;
}

static int master_poll(struct file *f, int events)
{
    struct pty *p = pty_of(f);
    int r = 0;
    if (p->out_count) r |= POLLIN;
    if (p->slave_seen && !p->slaves) r |= POLLHUP | POLLIN;
    if (p->in_count < IN_BUF) r |= POLLOUT;
    return r & (events | POLLHUP | POLLERR);
}

static void master_release(struct file *f)
{
    struct pty *p = pty_of(f);
    uint64_t fl = irq_save();
    p->masters = 0;
    /* hang-up: the session's foreground group and its leader get SIGHUP */
    if (p->session) {
        group_signal(p, SIGHUP);
        struct tcb *lead = task_find(p->session);
        if (lead && lead->state != TASK_ZOMBIE && lead->user) signal_send(lead, SIGHUP);
    }
    wakeup(&p->in);
    wakeup(&p->out);
    pty_free_if_unused(p);
    irq_restore(fl);
}

/* ---- slave side ------------------------------------------------------------ */
static ssize_t slave_read(struct file *f, void *buf, size_t len)
{
    struct pty *p = pty_of(f);
    if (!len) return 0;
    uint64_t fl = irq_save();
    uint8_t vmin = p->tio.c_cc[VMIN], vtime = p->tio.c_cc[VTIME];
    uint64_t deadline = (!canon(p) && vtime) ? time_ms() + (uint64_t)vtime * 100 : 0;
    for (;;) {
        size_t avail = slave_readable(p);
        if (canon(p) ? avail > 0 : (avail >= (vmin ? (vmin < len ? vmin : len) : 1) ||
                                    (avail && vtime) || (!vmin && !vtime)))
            break;
        if (!p->masters) { irq_restore(fl); return 0; } /* hung up: EOF */
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        if (deadline) {                                 /* VTIME: poll the clock */
            if (time_ms() >= deadline) break;
            irq_restore(fl);
            task_sleep_ms(10);
            fl = irq_save();
        } else {
            sleep_on(&p->in);
        }
    }
    size_t n;
    if (canon(p)) {
        if (!p->lq_count) { irq_restore(fl); return 0; }
        size_t line = p->lineq[p->lq_head];
        n = len < line ? len : line;
        if (n == line) { p->lq_head = (p->lq_head + 1) % LINEQ; p->lq_count--; }
        else p->lineq[p->lq_head] = (uint16_t)(line - n);
    } else {
        n = len < p->in_count ? len : p->in_count;
    }
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = p->in[(p->in_head + i) % IN_BUF];
    p->in_head = (p->in_head + n) % IN_BUF;
    p->in_count -= n;
    wakeup(&p->in);                                     /* master writers waiting for room */
    irq_restore(fl);
    return (ssize_t)n;
}

static ssize_t slave_write(struct file *f, const void *buf, size_t len)
{
    struct pty *p = pty_of(f);
    uint64_t fl = irq_save();
    size_t done = 0;
    while (done < len) {
        if (!p->masters) { irq_restore(fl); return done ? (ssize_t)done : -EIO; }
        if (p->out_count + 2 > OUT_BUF) {               /* room for "\r\n" */
            if (f->flags & O_NONBLOCK) break;
            if (signal_pending()) { irq_restore(fl); return done ? (ssize_t)done : -ERESTARTSYS; }
            wakeup(&p->out);
            sleep_on(&p->out);
            continue;
        }
        out_char(p, ((const uint8_t *)buf)[done++]);
    }
    wakeup(&p->out);
    irq_restore(fl);
    return done ? (ssize_t)done : -EAGAIN;
}

static int slave_poll(struct file *f, int events)
{
    struct pty *p = pty_of(f);
    int r = 0;
    if (slave_readable(p)) r |= POLLIN;
    if (!p->masters) r |= POLLHUP | POLLIN;
    if (p->out_count + 2 <= OUT_BUF) r |= POLLOUT;
    return r & (events | POLLHUP | POLLERR);
}

static void slave_release(struct file *f)
{
    struct pty *p = pty_of(f);
    uint64_t fl = irq_save();
    if (p->slaves) p->slaves--;
    wakeup(&p->out);                                    /* master readers see EIO/HUP */
    pty_free_if_unused(p);
    irq_restore(fl);
}

/* ---- ioctls (both sides) ------------------------------------------------- */
static bool uptr(void *arg, size_t len)
{
    struct tcb *t = current_task();
    return (uint64_t)arg >= 4096 && (!t->user || uvm_mapped(t->pml4, (uint64_t)arg, len));
}

static int pty_ioctl(struct file *f, unsigned long req, void *arg)
{
    struct pty *p = pty_of(f);
    bool master = f->vn->ops == &master_ops;
    struct tcb *t = current_task();
    switch (req) {
    case TCGETS:
        if (!uptr(arg, sizeof(struct ktermios))) return -EFAULT;
        *(struct ktermios *)arg = p->tio;
        return 0;
    case TCSETS: case TCSETSW: case TCSETSF: {
        if (!uptr(arg, sizeof(struct ktermios))) return -EFAULT;
        struct ktermios n = *(const struct ktermios *)arg;
        uint64_t fl = irq_save();
        if (req == TCSETSF) flush_input(p);
        set_termios(p, &n);
        wakeup(&p->in);
        irq_restore(fl);
        return 0;
    }
    case TIOCGWINSZ:
        if (!uptr(arg, sizeof(struct kwinsize))) return -EFAULT;
        *(struct kwinsize *)arg = p->ws;
        return 0;
    case TIOCSWINSZ: {
        if (!uptr(arg, sizeof(struct kwinsize))) return -EFAULT;
        struct kwinsize w = *(const struct kwinsize *)arg;
        bool changed = memcmp(&w, &p->ws, sizeof w) != 0;
        p->ws = w;
        if (changed) group_signal(p, SIGWINCH);
        return 0;
    }
    case TIOCGPTN:
        if (!master) return -ENOTTY;
        if (!uptr(arg, 4)) return -EFAULT;
        *(uint32_t *)arg = (uint32_t)p->index;
        return 0;
    case TIOCSPTLCK:
        if (!master) return -ENOTTY;
        if (!uptr(arg, 4)) return -EFAULT;
        p->locked = *(const int32_t *)arg != 0;
        return 0;
    case FIONREAD_T:
        if (!uptr(arg, 4)) return -EFAULT;
        *(int32_t *)arg = (int32_t)(master ? p->out_count : (canon(p) ? (p->lq_count ? p->lineq[p->lq_head] : 0) : p->in_count));
        return 0;
    case TIOCOUTQ:
        if (!uptr(arg, 4)) return -EFAULT;
        *(int32_t *)arg = master ? 0 : (int32_t)p->out_count;
        return 0;
    case TCFLSH: {
        long q = (long)arg;                             /* 0 input, 1 output, 2 both */
        uint64_t fl = irq_save();
        if (q == 0 || q == 2) flush_input(p);
        if (q == 1 || q == 2) p->out_head = p->out_count = 0;
        wakeup(&p->in); wakeup(&p->out);
        irq_restore(fl);
        return 0;
    }
    case TCSBRK: case TCSBRKP: case TCXONC:
        return 0;                                       /* nothing is ever pending */
    case TIOCSCTTY:
        if (master) return -ENOTTY;
        if (t->ctty == p->index + 1) return 0;
        if (t->sid != t->pid || t->ctty) return -EPERM;
        if (p->session && p->session != t->sid && (long)arg != 1) return -EPERM;
        acquire_ctty(p);
        return 0;
    case TIOCNOTTY:
        if (t->ctty != p->index + 1) return -ENOTTY;
        t->ctty = 0;
        if (t->sid == t->pid) { group_signal(p, SIGHUP); p->session = p->fg_pgrp = 0; }
        return 0;
    case TIOCGPGRP:
        if (!uptr(arg, 4)) return -EFAULT;
        if (!master && t->ctty != p->index + 1) return -ENOTTY;
        *(int32_t *)arg = p->fg_pgrp;
        return 0;
    case TIOCSPGRP: {
        if (!uptr(arg, 4)) return -EFAULT;
        if (master || t->ctty != p->index + 1) return -ENOTTY;
        int32_t g = *(const int32_t *)arg;
        if (g <= 0) return -EINVAL;
        p->fg_pgrp = g;
        return 0;
    }
    case TIOCGSID:
        if (!uptr(arg, 4)) return -EFAULT;
        if (!p->session) return -ENOTTY;
        *(int32_t *)arg = p->session;
        return 0;
    case TIOCGETD:
        if (!uptr(arg, 4)) return -EFAULT;
        *(int32_t *)arg = 0;                            /* N_TTY */
        return 0;
    case TIOCSETD:
        return 0;
    case FIONBIO:
        if (!uptr(arg, 4)) return -EFAULT;
        if (*(const int32_t *)arg) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        return 0;
    default:
        return -ENOTTY;
    }
}

static const struct vnode_ops master_ops = {
    .fread = master_read, .fwrite = master_write, .poll = master_poll, .release = master_release,
    .fioctl = pty_ioctl,
};
static const struct vnode_ops slave_ops = {
    .open = slave_open, .fread = slave_read, .fwrite = slave_write, .poll = slave_poll,
    .release = slave_release, .fioctl = pty_ioctl,
};
static const struct vnode_ops ptmx_ops = { .open = ptmx_open };

/* /dev/tty for a process whose controlling terminal is a pty. */
int pty_open_ctty(int flags, struct file **out)
{
    struct tcb *t = current_task();
    if (!t->ctty || t->ctty > PTY_MAX) return -ENXIO;
    struct pty *p = &ptys[t->ctty - 1];
    if (!p->used || !p->masters) return -EIO;
    struct file *f = vfs_file_new(pts_nodes[p->index], flags);
    if (!f) return -ENOMEM;
    p->slaves++;
    *out = f;
    return 0;
}

void pty_init(struct vnode *devroot)
{
    struct vnode *ptmx = vfs_node_new(devroot->fs, "ptmx", VCHR, 0666, &ptmx_ops);
    ptmx->rdev = (5u << 8) | 2;
    vfs_node_add(devroot, ptmx);
    struct vnode *dir = vfs_node_new(devroot->fs, "pts", VDIR, 0755, devroot->ops);
    vfs_node_add(devroot, dir);
    for (int i = 0; i < PTY_MAX; i++) {
        char name[8];
        snprintf(name, sizeof name, "%d", i);
        struct vnode *n = vfs_node_new(devroot->fs, name, VCHR, 0620, &slave_ops);
        n->rdev = (136u << 8) | (uint32_t)i;
        n->ctx = (void *)(intptr_t)i;
        vfs_node_add(dir, n);
        pts_nodes[i] = n;
    }
    kprintf("pty: /dev/ptmx and /dev/pts/0-%d\n", PTY_MAX - 1);
}
