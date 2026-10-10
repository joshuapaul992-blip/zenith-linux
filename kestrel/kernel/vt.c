/* kernel/vt.c -- a Unix shell on every text terminal (see vt.h)
 *
 * Per TTY, three kernel threads around a console pty (fs/pty.c), playing the
 * part a terminal emulator plays for a pty in X:
 *
 *   vt-in/ttyN    the TTY's key queue -> bytes (Enter = CR, Backspace = DEL,
 *                 cursor and editing keys as Linux console escape sequences,
 *                 Alt = ESC prefix) -> the pty's input side (line
 *                 discipline: echo, editing, Ctrl+C/Z/\ -> signals)
 *   vt-out/ttyN   the pty's output -> tty_write_vt() (escape sequences,
 *                 UTF-8) -> this monitor
 *   getty/ttyN    starts "-sh" (/bin/sh as a login shell, which reads
 *                 /etc/profile) on /dev/ttyN as a new session with it as
 *                 the controlling terminal; when the shell exits, hangs the
 *                 terminal up (SIGHUP for what is left of the session),
 *                 resets it and starts the next one.
 *
 * The terminal's answers to status queries (ESC[6n and friends) are typed
 * back into the pty through the TTY's reply hook. */
#include <kernel/vt.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/exec.h>
#include <kernel/task.h>
#include <kernel/bootinfo.h>
#include <kernel/klog.h>
#include <kernel/arch.h>
#include <kernel/string.h>

struct vt {
    struct kestrel_tty *tty;
    int pty;
};
static struct vt vts[MAX_MONITORS];

bool vt_shell_available(void)
{
    struct stat st;
    return !strstr(g_boot.cmdline, "kestrel.shell=kush") && vfs_stat("/bin/sh", &st) == 0;
}

static void vt_reply(void *ctx, const char *s, size_t n)
{
    pty_console_input(((struct vt *)ctx)->pty, s, n);
}

static int vt_output(void *arg)
{
    struct vt *v = arg;
    char buf[512];
    for (;;) {
        ssize_t n = pty_console_read(v->pty, buf, sizeof buf);
        if (n > 0) tty_write_vt(v->tty, buf, (size_t)n);
        else task_sleep_ms(10);
    }
    return 0;
}

/* One key as the bytes a Linux console sends for it. */
static size_t key_bytes(const struct vt *v, const struct key_event *ev, char *out)
{
    static const char *const fkeys[12] = {
        "\033[[A", "\033[[B", "\033[[C", "\033[[D", "\033[[E", "\033[17~",
        "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
    };
    const char *seq = NULL;
    switch (ev->key) {
    case KEY_ENTER:     seq = "\r"; break;
    case KEY_TAB:       seq = (ev->mods & MOD_SHIFT) ? "\033[Z" : "\t"; break;
    case KEY_ESC:       seq = "\033"; break;
    case KEY_BACKSPACE: seq = "\177"; break;
    case KEY_UP:        seq = v->tty->app_cursor ? "\033OA" : "\033[A"; break;
    case KEY_DOWN:      seq = v->tty->app_cursor ? "\033OB" : "\033[B"; break;
    case KEY_RIGHT:     seq = v->tty->app_cursor ? "\033OC" : "\033[C"; break;
    case KEY_LEFT:      seq = v->tty->app_cursor ? "\033OD" : "\033[D"; break;
    case KEY_HOME:      seq = "\033[1~"; break;
    case KEY_INSERT:    seq = "\033[2~"; break;
    case KEY_DELETE:    seq = "\033[3~"; break;
    case KEY_END:       seq = "\033[4~"; break;
    case KEY_PGUP:      seq = "\033[5~"; break;
    case KEY_PGDN:      seq = "\033[6~"; break;
    default:
        if (ev->key >= KEY_F1 && ev->key <= KEY_F12) { seq = fkeys[ev->key - KEY_F1]; break; }
        if (!ev->ascii) return 0;
        size_t n = 0;
        if (ev->mods & MOD_ALT) out[n++] = '\033';      /* Meta sends ESC first */
        out[n++] = ev->ascii;
        return n;
    }
    size_t n = 0;
    if ((ev->mods & MOD_ALT) && seq[0] != '\033') out[n++] = '\033';
    size_t l = strlen(seq);
    memcpy(out + n, seq, l);
    return n + l;
}

static int vt_input(void *arg)
{
    struct vt *v = arg;
    for (;;) {
        struct key_event ev;
        if (!tty_read_key(v->tty, &ev, true)) { task_sleep_ms(10); continue; }
        char buf[16];
        size_t n = key_bytes(v, &ev, buf);
        if (n) pty_console_input(v->pty, buf, n);
    }
    return 0;
}

static bool vt_setup(int index, struct vt *v)
{
    char path[16];
    snprintf(path, sizeof path, "/dev/tty%d", index + 1);
    struct vnode *node;
    v->tty = tty_get(index);
    if (!v->tty || vfs_lookup(path, &node) < 0) return false;
    v->pty = pty_console_create(node, v->tty->max_rows, v->tty->max_cols);
    if (v->pty < 0) { kprintf("vt: %s: no pty (%d)\n", path, v->pty); return false; }
    v->tty->reply_ctx = v;
    v->tty->reply = vt_reply;
    char name[TASK_NAME_LEN];
    snprintf(name, sizeof name, "vt-out/tty%d", index + 1);
    struct tcb *out = task_create(name, vt_output, v);
    snprintf(name, sizeof name, "vt-in/tty%d", index + 1);
    struct tcb *in = task_create(name, vt_input, v);
    if (!out || !in) { kprintf("vt: %s: cannot start its threads\n", path); return false; }
    kprintf("vt: %s is a terminal (%d x %d), /dev/pts/%d underneath\n", path, v->tty->max_cols,
            v->tty->max_rows, v->pty);
    return true;
}

void vt_session(int index)
{
    struct vt *v = &vts[index];
    tty_bind_current(index);                            /* the shells inherit this TTY */
    if (!vt_setup(index, v)) {
        kprintf("vt: tty%d: no terminal, nothing runs on it\n", index + 1);
        for (;;) task_sleep_ms(1000000);
    }
    char dev[16];
    snprintf(dev, sizeof dev, "/dev/tty%d", index + 1);
    char *argv[] = { "-sh" };                           /* a login shell: reads /etc/profile */
    int quick = 0;
    for (;;) {
        uint64_t start = uptime_ms();
        int pid = exec_spawn_tty("/bin/sh", 1, argv, dev);
        int st = pid < 0 ? pid : exec_wait(pid);
        pty_console_hangup(v->pty);
        task_sleep_ms(50);                              /* let its last output reach the screen */
        tty_reset(v->tty, false);
        char msg[96];
        int n = snprintf(msg, sizeof msg, "\r\n\033[90m[sh %s %d; a new shell starts]\033[0m\r\n",
                         pid < 0 ? "could not start:" : "exited with status", st);
        tty_write_vt(v->tty, msg, (size_t)n);
        if (uptime_ms() - start < 2000 && ++quick >= 3) {   /* failing at once: do not spin */
            static const char wait_msg[] = "\033[90m[retrying in 10 s]\033[0m\r\n";
            tty_write_vt(v->tty, wait_msg, sizeof wait_msg - 1);
            task_sleep_ms(10000);
            quick = 0;
        } else if (uptime_ms() - start >= 2000) {
            quick = 0;
        }
    }
}
