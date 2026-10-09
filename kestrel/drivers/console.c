/* drivers/console.c -- emulated "high-resolution text mode" over the LFB */
#include <kernel/console.h>
#include <kernel/fb.h>
#include <kernel/string.h>
#include <kernel/cpu.h>

struct console g_con;
static void (*con_redirect)(const char *s, size_t n);

void con_set_redirect(void (*write)(const char *s, size_t n)) { con_redirect = write; }

static const uint32_t ansi_colors[16] = {
    RGB(0x00,0x00,0x00), RGB(0xCD,0x31,0x31), RGB(0x0D,0xBC,0x79), RGB(0xE5,0xE5,0x10),
    RGB(0x24,0x72,0xC8), RGB(0xBC,0x3F,0xBC), RGB(0x11,0xA8,0xCD), RGB(0xC0,0xC0,0xC0),
    RGB(0x66,0x66,0x66), RGB(0xF1,0x4C,0x4C), RGB(0x23,0xD1,0x8B), RGB(0xF5,0xF5,0x43),
    RGB(0x3B,0x8E,0xEA), RGB(0xD6,0x70,0xD6), RGB(0x29,0xB8,0xDB), RGB(0xFF,0xFF,0xFF),
};

static void mark_dirty(struct console *c, int row)
{
    if (row < c->dirty_top) c->dirty_top = row;
    if (row > c->dirty_bot) c->dirty_bot = row;
}

static void draw_cursor(struct console *c, bool on)
{
    if (!c->cursor_visible || c->cx >= c->cols) return;
    int px = c->x0 + c->cx * c->font->width;
    int py = c->y0 + c->cy * c->font->height;
    draw_rect(px, py + c->font->height - 2, c->font->width, 2, on ? c->fg : c->bg);
    mark_dirty(c, c->cy);
}

void con_init(struct console *c, const struct font *f, int x0, int y0, int cols, int rows,
              uint32_t fg, uint32_t bg)
{
    memset(c, 0, sizeof *c);
    c->font = f; c->x0 = x0; c->y0 = y0; c->cols = cols; c->rows = rows;
    c->fg = c->default_fg = fg; c->bg = bg;
    c->dirty_top = rows; c->dirty_bot = -1;
}

void con_flush(struct console *c)
{
    if (c->dirty_bot < c->dirty_top) return;
    fb_flush_rect(c->x0, c->y0 + c->dirty_top * c->font->height,
                  c->cols * c->font->width, (c->dirty_bot - c->dirty_top + 1) * c->font->height);
    c->dirty_top = c->rows; c->dirty_bot = -1;
}

void con_clear(struct console *c)
{
    if (c == &g_con && con_redirect) { con_redirect("\033[2J", 4); return; }
    draw_rect(c->x0, c->y0, c->cols * c->font->width, c->rows * c->font->height, c->bg);
    c->cx = c->cy = 0;
    c->dirty_top = 0; c->dirty_bot = c->rows - 1;
}

void con_set_color(struct console *c, uint32_t fg, uint32_t bg) { c->fg = fg; c->bg = bg; }

static void newline(struct console *c)
{
    c->cx = 0;
    if (++c->cy >= c->rows) {
        fb_scroll(c->y0, c->rows * c->font->height, c->font->height, c->bg);
        c->cy = c->rows - 1;
        c->dirty_top = 0; c->dirty_bot = c->rows - 1;
    }
}

static void apply_sgr(struct console *c, int v)
{
    if (v == 0)                   c->fg = c->default_fg;
    else if (v == 1)              { /* bold: brighten */ }
    else if (v >= 30 && v <= 37)  c->fg = ansi_colors[v - 30];
    else if (v >= 90 && v <= 97)  c->fg = ansi_colors[v - 90 + 8];
    else if (v == 39)             c->fg = c->default_fg;
}

static bool handle_escape(struct console *c, char ch)
{
    if (c->esc_state == 1) {                 /* got ESC */
        if (ch == '[') { c->esc_state = 2; c->esc_n = 0; c->esc_val = -1; return true; }
        c->esc_state = 0; return true;
    }
    /* esc_state == 2: CSI parameters */
    if (ch >= '0' && ch <= '9') { c->esc_val = (c->esc_val < 0 ? 0 : c->esc_val) * 10 + (ch - '0'); return true; }
    if (ch == ';') { if (c->esc_n < 4) c->esc_args[c->esc_n++] = c->esc_val < 0 ? 0 : c->esc_val; c->esc_val = -1; return true; }
    if (c->esc_n < 4) c->esc_args[c->esc_n++] = c->esc_val < 0 ? 0 : c->esc_val;
    switch (ch) {
    case 'm': for (int i = 0; i < c->esc_n; i++) apply_sgr(c, c->esc_args[i]); break;
    case 'J': if (c->esc_args[0] == 2) con_clear(c); break;
    case 'H': c->cx = c->cy = 0; break;
    default: break;
    }
    c->esc_state = 0;
    return true;
}

void con_putc(struct console *c, char ch)
{
    if (c->esc_state) { handle_escape(c, ch); return; }

    switch (ch) {
    case '\033': c->esc_state = 1; return;
    case '\n': newline(c); return;
    case '\r': c->cx = 0; return;
    case '\t': do con_putc(c, ' '); while (c->cx % 8); return;
    case '\b':
        if (c->cx > 0) c->cx--;
        else if (c->cy > 0) { c->cy--; c->cx = c->cols - 1; }
        return;
    default: break;
    }
    if (c->cx >= c->cols) newline(c);
    fb_draw_char(c->x0 + c->cx * c->font->width, c->y0 + c->cy * c->font->height,
              ch, c->fg, c->bg, c->font);
    mark_dirty(c, c->cy);
    c->cx++;
}

void con_write(struct console *c, const char *s, size_t n)
{
    if (c == &g_con && con_redirect) { con_redirect(s, n); return; }
    uint64_t f = irq_save();            /* keep a line atomic w.r.t. other threads */
    draw_cursor(c, false);
    for (size_t i = 0; i < n; i++) con_putc(c, s[i]);
    draw_cursor(c, true);
    con_flush(c);
    irq_restore(f);
}

void con_puts(struct console *c, const char *s) { con_write(c, s, strlen(s)); }

void con_printf(struct console *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_write(c, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void con_show_cursor(struct console *c, bool on)
{
    if (c == &g_con && con_redirect) return;    /* the terminal owns its cursor */
    if (!on) draw_cursor(c, false);
    c->cursor_visible = on;
    if (on) draw_cursor(c, true);
    con_flush(c);
}
