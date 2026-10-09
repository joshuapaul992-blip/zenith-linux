/* include/kernel/console.h -- text-mode emulation on top of the frame buffer */
#ifndef KESTREL_CONSOLE_H
#define KESTREL_CONSOLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <kernel/font.h>

/* A console is a grid of character cells rendered with a bitmap font
 * (128x48 cells with the 8x16 font at 1024x768). It understands \n \r \b \t
 * and the ANSI sequences ESC[<n>m (colours 30-37/90-97, 0 reset),
 * ESC[2J (clear) and ESC[H (home), which is enough for a POSIX tty. */
struct console {
    const struct font *font;
    int x0, y0;             /* pixel origin                  */
    int cols, rows;
    int cx, cy;             /* cursor cell                   */
    uint32_t fg, bg, default_fg;
    bool cursor_visible;
    int esc_state; int esc_val; int esc_n; int esc_args[4];
    int dirty_top, dirty_bot;
};

extern struct console g_con;

void con_init(struct console *c, const struct font *f, int x0, int y0, int cols, int rows,
              uint32_t fg, uint32_t bg);
void con_putc(struct console *c, char ch);
void con_write(struct console *c, const char *s, size_t n);
void con_puts(struct console *c, const char *s);
void con_printf(struct console *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void con_clear(struct console *c);
void con_set_color(struct console *c, uint32_t fg, uint32_t bg);
void con_show_cursor(struct console *c, bool on);
void con_flush(struct console *c);
/* Route everything written to g_con (kernel log mirror, /dev/tty) to another
 * output, e.g. the interactive terminal. NULL restores normal rendering. */
void con_set_redirect(void (*write)(const char *s, size_t n));

#endif
