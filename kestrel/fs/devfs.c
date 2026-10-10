/* fs/devfs.c -- /dev character devices (major/minor numbers follow Linux) */
#include <kernel/kfb.h>
#include <kernel/task.h>
#include <kernel/display.h>
#include <kernel/uvm.h>
#include <kernel/vfs.h>
#include <kernel/console.h>
#include <kernel/keyboard.h>
#include <kernel/serial.h>
#include <kernel/fb.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/tty.h>

#define MKDEV(ma, mi) (((ma) << 8) | (mi))

/* ---- null / zero / random --------------------------------------------- */
static ssize_t null_read(struct vnode *v, void *b, size_t n, uint64_t o)        { (void)v; (void)b; (void)n; (void)o; return 0; }
static ssize_t sink_write(struct vnode *v, const void *b, size_t n, uint64_t o) { (void)v; (void)b; (void)o; return (ssize_t)n; }
static ssize_t zero_read(struct vnode *v, void *b, size_t n, uint64_t o)        { (void)v; (void)o; memset(b, 0, n); return (ssize_t)n; }

static uint64_t rng_state;
static ssize_t random_read(struct vnode *v, void *b, size_t n, uint64_t o)
{
    (void)v; (void)o;
    if (!rng_state) rng_state = rdtsc() | 1;
    uint8_t *p = b;
    for (size_t i = 0; i < n; i++) {
        rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
        p[i] = (uint8_t)(rng_state >> 24);
    }
    return (ssize_t)n;
}

static const struct vnode_ops null_ops   = { .read = null_read,   .write = sink_write };
static const struct vnode_ops zero_ops   = { .read = zero_read,   .write = sink_write };
static const struct vnode_ops random_ops = { .read = random_read, .write = sink_write };

/* ---- tty: one terminal per monitor --------------------------------------
 *
 *   /dev/tty         the calling task's TTY (shell, its utilities)
 *   /dev/console     TTY 0, the system console
 *   /dev/tty1 ...    the TTY of monitor 0, 1, ... (Linux numbering: Ctrl+Alt+F1 = tty1)
 *
 * The line discipline lives in tty.c and keeps its line in that TTY's own
 * input_buffer. Without any TTY (no usable frame buffer) the text console
 * g_con and the global keyboard queue are used instead. */

static struct kestrel_tty *vnode_tty(struct vnode *v) { return v->ctx ? v->ctx : tty_current(); }

static char   con_line[256];
static size_t con_len, con_pos;     /* bytes in completed line / already consumed */
static bool   con_ready;

static void tty_echo(const char *s) { con_puts(&g_con, s); }

static ssize_t con_read(void *buf, size_t n)
{
    if (!con_ready) {                       /* gather one edited line */
        con_len = con_pos = 0;
        con_show_cursor(&g_con, true);
        for (;;) {
            struct key_event ev;
            keyboard_wait(&ev);
            char c = ev.ascii;
            if (c == '\n') { con_line[con_len++] = '\n'; tty_echo("\n"); break; }
            if (c == '\b') { if (con_len) { con_len--; tty_echo("\b \b"); } continue; }
            if (c == 3)    { tty_echo("^C\n"); con_len = 0; con_line[con_len++] = '\n'; break; }   /* Ctrl+C */
            if (c == 4)    { if (con_len == 0) { con_show_cursor(&g_con, false); return 0; } continue; } /* Ctrl+D = EOF */
            if (c == 12)   { con_puts(&g_con, "\033[2J"); continue; }                                     /* Ctrl+L */
            if (c == 21)   { while (con_len) { con_len--; tty_echo("\b \b"); } continue; }               /* Ctrl+U */
            if (c >= ' ' && c < 127 && con_len < sizeof con_line - 1) {
                con_line[con_len++] = c;
                char e[2] = { c, 0 };
                tty_echo(e);
            }
        }
        con_show_cursor(&g_con, false);
        con_ready = true;
    }
    size_t avail = con_len - con_pos;
    if (n > avail) n = avail;
    memcpy(buf, con_line + con_pos, n);
    con_pos += n;
    if (con_pos >= con_len) con_ready = false;
    return (ssize_t)n;
}

static ssize_t devtty_read(struct vnode *v, void *buf, size_t n, uint64_t o)
{
    (void)o;
    struct kestrel_tty *t = vnode_tty(v);
    return t ? (ssize_t)tty_read_line(t, buf, n) : con_read(buf, n);
}

static ssize_t devtty_write(struct vnode *v, const void *buf, size_t n, uint64_t o)
{
    (void)o;
    struct kestrel_tty *t = vnode_tty(v);
    if (t) tty_write(t, buf, n);
    else con_write(&g_con, buf, n);
    return (ssize_t)n;
}

static int devtty_ioctl(struct vnode *v, unsigned long req, void *arg)
{
    if (req == TIOCGWINSZ) {                /* the size of this TTY's own monitor */
        struct winsize *ws = arg;
        struct kestrel_tty *t = vnode_tty(v);
        if (t) {
            ws->ws_row = (uint16_t)t->max_rows; ws->ws_col = (uint16_t)t->max_cols;
            ws->ws_xpixel = (uint16_t)t->native_width; ws->ws_ypixel = (uint16_t)t->native_height;
        } else {
            ws->ws_row = (uint16_t)g_con.rows; ws->ws_col = (uint16_t)g_con.cols;
            ws->ws_xpixel = (uint16_t)(g_con.cols * g_con.font->width);
            ws->ws_ypixel = (uint16_t)(g_con.rows * g_con.font->height);
        }
        return 0;
    }
    return -ENOTTY;
}

static const struct vnode_ops tty_ops     = { .read = devtty_read, .write = devtty_write, .ioctl = devtty_ioctl };

/* /dev/tty: the controlling terminal. A process in a pty session gets its
 * pty slave; everything else keeps Kestrel's own terminal (t->tty). */
static int devtty_open(struct vnode *vn, int flags, struct file **out)
{
    if (current_task()->ctty) return pty_open_ctty(flags, out);
    struct file *f = vfs_file_new(vn, flags);
    if (!f) return -ENOMEM;
    *out = f;
    return 0;
}
static const struct vnode_ops ctty_ops    = { .open = devtty_open, .read = devtty_read, .write = devtty_write,
                                              .ioctl = devtty_ioctl };
static const struct vnode_ops console_ops = { .read = null_read, .write = devtty_write, .ioctl = devtty_ioctl };

/* ---- serial ------------------------------------------------------------ */
static ssize_t serial_dev_write(struct vnode *v, const void *buf, size_t n, uint64_t o)
{
    (void)v; (void)o;
    const char *s = buf;
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
    return (ssize_t)n;
}
static const struct vnode_ops serial_ops = { .read = null_read, .write = serial_dev_write };

/* ---- kmsg -------------------------------------------------------------- */
static ssize_t kmsg_read(struct vnode *v, void *buf, size_t n, uint64_t off)
{
    (void)v;
    return (ssize_t)klog_read(off, buf, n);
}
static ssize_t kmsg_write(struct vnode *v, const void *buf, size_t n, uint64_t o)
{
    (void)v; (void)o;
    char tmp[256];
    for (size_t done = 0; done < n; ) {                 /* all of it, 255 bytes at a time */
        size_t k = n - done < sizeof tmp - 1 ? n - done : sizeof tmp - 1;
        memcpy(tmp, (const char *)buf + done, k); tmp[k] = 0;
        kprintf("%s", tmp);
        done += k;
    }
    return (ssize_t)n;
}
static uint64_t kmsg_size(struct vnode *v) { (void)v; return klog_size(); }
static const struct vnode_ops kmsg_ops = { .read = kmsg_read, .write = kmsg_write, .size = kmsg_size };

/* ---- fbN: one per monitor, for graphics clients (see kfb.h) ------------- */
static int fb_owner[MAX_MONITORS];          /* pid holding KD_GRAPHICS, 0 = none */

static int fb_ioctl(struct vnode *v, unsigned long req, void *arg)
{
    struct kestrel_tty *t = v->ctx;
    int idx = t->index;
    const struct display_head *h = display_get(idx);
    switch (req) {
    case KFB_GET_INFO: {
        if (!arg) return -EFAULT;
        struct kfb_info *i = arg;                       /* a kernel copy (sys_ioctl) */
        memset(i, 0, sizeof *i);
        i->width = (uint32_t)t->native_width; i->height = (uint32_t)t->native_height;
        i->pitch = (uint32_t)t->native_width * 4; i->bpp = (uint32_t)t->bytes_pp * 8;
        i->r_pos = t->r_pos; i->g_pos = t->g_pos; i->b_pos = t->b_pos; i->index = (uint8_t)idx;
        if (h) { strlcpy(i->name, h->name, sizeof i->name); strlcpy(i->monitor, h->monitor, sizeof i->monitor); }
        return 0;
    }
    case FBIOGET_VSCREENINFO: {
        if (!arg) return -EFAULT;
        struct fb_var_screeninfo_lite *si = arg;
        si->xres = (uint32_t)t->native_width; si->yres = (uint32_t)t->native_height;
        si->bits_per_pixel = (uint32_t)t->bytes_pp * 8; si->line_length = (uint32_t)t->native_width * 4;
        si->smem_start = h ? h->phys : 0;
        return 0;
    }
    case KDSETMODE: {
        uint64_t mode = (uint64_t)arg;
        int pid = current_task()->pid;
        if (mode == KD_GRAPHICS) {
            if (fb_owner[idx] && fb_owner[idx] != pid) return -EBUSY;
            int rc = tty_set_graphics(idx, true);
            if (rc == 0) fb_owner[idx] = pid;
            return rc;
        }
        if (mode == KD_TEXT) {
            if (fb_owner[idx] && fb_owner[idx] != pid) return -EBUSY;
            fb_owner[idx] = 0;
            return tty_set_graphics(idx, false);
        }
        return -EINVAL;
    }
    case KFB_BLIT: {
        if (!arg) return -EFAULT;
        const struct kfb_blit *b = arg;                 /* kernel copy; b->src is a user pointer */
        if (b->w <= 0 || b->h <= 0) return 0;
        if (b->w > 16384 || b->h > 16384 || b->src_pitch < (uint32_t)b->w * 4) return -EINVAL;
        return tty_blit_user(idx, b->x, b->y, b->w, b->h, b->src, b->src_pitch);
    }
    default:
        return -ENOTTY;
    }
}

/* The owner closing its last descriptor (or exiting) gives the text back. */
static void fb_release(struct file *f)
{
    struct kestrel_tty *t = f->vn->ctx;
    if (fb_owner[t->index] == current_task()->pid) {
        fb_owner[t->index] = 0;
        tty_set_graphics(t->index, false);
    }
}

static const struct vnode_ops fb_ops = { .ioctl = fb_ioctl, .release = fb_release };

/* ------------------------------------------------------------------------- */
static void add_dev(struct vnode *dir, const char *name, uint32_t mode, uint32_t rdev,
                    const struct vnode_ops *ops)
{
    struct vnode *d = vfs_node_new(dir->fs, name, VCHR, mode, ops);
    d->rdev = rdev;
    vfs_node_add(dir, d);
}

static void add_tty(struct vnode *dir, const char *name, uint32_t mode, uint32_t rdev,
                    const struct vnode_ops *ops, struct kestrel_tty *t)
{
    struct vnode *d = vfs_node_new(dir->fs, name, VCHR, mode, ops);
    d->rdev = rdev;
    d->ctx = t;
    vfs_node_add(dir, d);
}

static struct vnode *devfs_root;

struct vnode *devfs_register(const char *name, uint32_t mode, uint32_t rdev,
                             const struct vnode_ops *ops, void *ctx)
{
    if (!devfs_root) return NULL;
    struct vnode *d = vfs_node_new(devfs_root->fs, name, VCHR, mode, ops);
    d->rdev = rdev;
    d->ctx = ctx;
    vfs_node_add(devfs_root, d);
    return d;
}

void devfs_init(const char *mountpoint)
{
    struct mount *m;
    struct vnode *r = vfs_mkfs("devfs", MNT_NOCREATE, &m);
    devfs_root = r;
    add_dev(r, "null",    0666, MKDEV(1, 3),  &null_ops);
    add_dev(r, "zero",    0666, MKDEV(1, 5),  &zero_ops);
    add_dev(r, "random",  0666, MKDEV(1, 8),  &random_ops);
    add_dev(r, "urandom", 0666, MKDEV(1, 9),  &random_ops);
    add_dev(r, "kmsg",    0644, MKDEV(1, 11), &kmsg_ops);
    add_dev(r, "tty",     0666, MKDEV(5, 0),  &ctty_ops);
    add_tty(r, "console", 0600, MKDEV(5, 1),  &console_ops, tty_get(0));
    for (int i = 0; i < tty_count; i++) {
        char name[8];
        snprintf(name, sizeof name, "tty%d", i + 1);
        add_tty(r, name, 0620, MKDEV(4, (uint32_t)(i + 1)), &tty_ops, tty_get(i));
    }
    add_dev(r, "ttyS0",   0660, MKDEV(4, 64), &serial_ops);
    for (int i = 0; i < tty_count; i++) {              /* one per monitor */
        char name[8];
        snprintf(name, sizeof name, "fb%d", i);
        add_tty(r, name, 0660, MKDEV(29, (uint32_t)i), &fb_ops, tty_get(i));
    }
    pty_init(r);
    vfs_mount(m, mountpoint);
}
