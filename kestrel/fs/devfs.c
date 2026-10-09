/* fs/devfs.c -- /dev character devices (major/minor numbers follow Linux) */
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

/* ---- fb0: raw pixels of the linear frame buffer ------------------------- */
#define FBIOGET_VSCREENINFO 0x4600
struct fb_var_screeninfo_lite { uint32_t xres, yres, bits_per_pixel, line_length; uint64_t smem_start; };

static ssize_t fb_read(struct vnode *v, void *buf, size_t n, uint64_t off)
{
    (void)v;
    uint64_t sz = fb_size_bytes();
    if (off >= sz) return 0;
    if (n > sz - off) n = sz - off;
    memcpy(buf, g_fb.back + off, n);
    return (ssize_t)n;
}
static ssize_t fb_write(struct vnode *v, const void *buf, size_t n, uint64_t off)
{
    (void)v;
    uint64_t sz = fb_size_bytes();
    if (off >= sz) return -ENOSPC;
    if (n > sz - off) n = sz - off;
    memcpy(g_fb.back + off, buf, n);
    int y0 = (int)(off / g_fb.pitch), y1 = (int)((off + n + g_fb.pitch - 1) / g_fb.pitch);
    fb_flush_rect(0, y0, (int)g_fb.width, y1 - y0);
    return (ssize_t)n;
}
static int fb_ioctl(struct vnode *v, unsigned long req, void *arg)
{
    (void)v;
    if (req != FBIOGET_VSCREENINFO) return -ENOTTY;
    struct fb_var_screeninfo_lite *si = arg;
    si->xres = g_fb.width; si->yres = g_fb.height; si->bits_per_pixel = g_fb.bpp;
    si->line_length = g_fb.pitch; si->smem_start = g_fb.phys;
    return 0;
}
static uint64_t fb_dev_size(struct vnode *v) { (void)v; return fb_size_bytes(); }
static const struct vnode_ops fb_ops = { .read = fb_read, .write = fb_write, .ioctl = fb_ioctl, .size = fb_dev_size };

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
    add_dev(r, "tty",     0666, MKDEV(5, 0),  &tty_ops);
    add_tty(r, "console", 0600, MKDEV(5, 1),  &console_ops, tty_get(0));
    for (int i = 0; i < tty_count; i++) {
        char name[8];
        snprintf(name, sizeof name, "tty%d", i + 1);
        add_tty(r, name, 0620, MKDEV(4, (uint32_t)(i + 1)), &tty_ops, tty_get(i));
    }
    add_dev(r, "ttyS0",   0660, MKDEV(4, 64), &serial_ops);
    add_dev(r, "fb0",     0660, MKDEV(29, 0), &fb_ops);
    vfs_mount(m, mountpoint);
}
