/* kernel/klog.c -- kernel message log, serial mirror and panic() */
#include <kernel/klog.h>
#include <kernel/serial.h>
#include <kernel/console.h>
#include <kernel/fb.h>
#include <kernel/string.h>
#include <kernel/cpu.h>

static char   log_buf[KLOG_SIZE];
static size_t log_len;              /* total bytes ever written (wraps buffer) */
static bool   to_console;

void klog_set_console(bool on) { to_console = on; }

void klog_putc(char c)
{
    static char line[192];
    static size_t len;
    if (c == '\n' || len == sizeof line - 1) {
        line[len] = 0;
        kprintf("%s\n", line);
        len = 0;
        if (c == '\n') return;
    }
    line[len++] = c;
}
bool klog_console_enabled(void) { return to_console; }

static void log_append(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        log_buf[(log_len + i) % KLOG_SIZE] = s[i];
    log_len += n;
}

size_t klog_size(void) { return log_len < KLOG_SIZE ? log_len : KLOG_SIZE; }

size_t klog_read(size_t off, char *buf, size_t len)
{
    size_t avail = klog_size();
    size_t start = log_len - avail;           /* oldest byte still present */
    if (off >= avail) return 0;
    if (len > avail - off) len = avail - off;
    for (size_t i = 0; i < len; i++) buf[i] = log_buf[(start + off + i) % KLOG_SIZE];
    return len;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf) n = sizeof buf - 1;

    uint64_t f = irq_save();
    log_append(buf, (size_t)n);
    serial_write(buf);
    irq_restore(f);
    if (to_console && g_fb.ready) con_write(&g_con, buf, (size_t)n);
}

void panic(const char *fmt, ...)
{
    cli();
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    serial_write("\n*** KERNEL PANIC: ");
    serial_write(buf);
    serial_write("\n");

    if (g_fb.ready) {
        int w = 900, h = 120, x = ((int)g_fb.width - w) / 2, y = ((int)g_fb.height - h) / 2;
        draw_rect(x, y, w, h, RGB(0x80, 0x00, 0x00));
        draw_rect_outline(x, y, w, h, 2, COL_WHITE);
        draw_string(x + 16, y + 16, "*** KERNEL PANIC ***", COL_WHITE, FB_TRANSPARENT, &font_12x24);
        draw_string(x + 16, y + 52, buf, COL_WHITE, FB_TRANSPARENT, &font_8x16);
        draw_string(x + 16, y + 84, "The system has been halted. Restart your computer.",
                    COL_SILVER, FB_TRANSPARENT, &font_8x16);
        fb_flush();
    }
    cpu_halt_forever();
}
