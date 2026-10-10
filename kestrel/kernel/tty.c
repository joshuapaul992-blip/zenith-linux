/* kernel/tty.c -- per-monitor text terminals (see tty.h)
 *
 * Layers (each a separate group of functions below):
 *   1. pixels       surface/publish helpers and draw_char(): every address is
 *                   computed from the TTY's own frame buffer, pitch and size
 *   2. cell model   a shadow copy of every cell (character + colours) so the
 *                   cursor can restore what it covers and scrolling keeps the
 *                   model in step with the pixels
 *   3. cursor       hide/show/blink per TTY; tty_cursor_tick() walks all TTYs
 *                   from the timer interrupt
 *   4. text layout  putc, wrapping, newline, scroll_screen, clear, ANSI
 *   5. input        focus hotkeys and key routing (keyboard ISR), per-TTY key
 *                   queues, the /dev/tty line discipline
 *
 * Monitors with a RAM shadow (display_head.shadow) are drawn and scrolled in
 * the shadow and the changed rectangle is then copied to VRAM, because
 * reading VRAM over PCIe is slow; without one, the same code works on VRAM. */
#include <kernel/process.h>
#include <kernel/tty.h>
#include <kernel/posix.h>
#include <kernel/kestrel_font.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>

struct kestrel_tty system_ttys[MAX_MONITORS];
int tty_count;
volatile int active_keyboard_tty;

static bool input_active;               /* keys go to TTY queues, hotkeys enabled */

/* ======================================================================== */
/*  0. per-TTY lock                                                            */
/* ======================================================================== */

static bool tty_trylock(struct kestrel_tty *t) { return !__atomic_exchange_n(&t->lock, 1, __ATOMIC_ACQUIRE); }
static void tty_unlock(struct kestrel_tty *t) { __atomic_store_n(&t->lock, 0, __ATOMIC_RELEASE); }

/* Wait for the TTY, giving the CPU to its holder. With interrupts disabled
 * the holder cannot run, so a busy TTY is reported instead (false). */
static bool tty_lock(struct kestrel_tty *t)
{
    while (!tty_trylock(t)) {
        if (!irqs_enabled()) return false;
        if (sched_running()) sched_yield(); else cpu_relax();
    }
    return true;
}

/* ======================================================================== */
/*  1. pixels                                                                  */
/* ======================================================================== */

/* Where drawing happens: the RAM shadow if the monitor has one, else VRAM. */
static inline uint32_t *surface(const struct kestrel_tty *t)
{
    return t->shadow ? t->shadow : t->framebuffer_address;
}

static inline uint32_t native_color(const struct kestrel_tty *t, uint32_t rgb)
{
    return ((rgb >> 16) & 0xFFu) << t->r_pos | ((rgb >> 8) & 0xFFu) << t->g_pos | (rgb & 0xFFu) << t->b_pos;
}

/* Scan line `py` of this monitor's drawing surface. */
static inline uint8_t *row_ptr(const struct kestrel_tty *t, int py)
{
    return (uint8_t *)surface(t) + (size_t)py * (size_t)t->pitch;
}

static inline void put_px(const struct kestrel_tty *t, uint8_t *row, int px, uint32_t v)
{
    if (t->bytes_pp == 4) {
        ((uint32_t *)row)[px] = v;
    } else {                                                    /* packed 24-bpp VBE modes */
        uint8_t *p = row + (size_t)px * 3;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
    }
}

/* Copy a rectangle of this monitor's shadow to its VRAM (no-op without a
 * shadow: drawing already went to VRAM). Clipped to the monitor. */
static void publish_now(const struct kestrel_tty *t, int x, int y, int w, int h)
{
    if (!t->shadow) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->native_width)  w = t->native_width - x;
    if (y + h > t->native_height) h = t->native_height - y;
    if (w <= 0 || h <= 0) return;
    const size_t bpp = (size_t)t->bytes_pp;
    for (int py = y; py < y + h; py++) {
        size_t off = (size_t)py * (size_t)t->pitch + (size_t)x * bpp;
        if (t->write_span) {
            t->write_span(t->write_ctx, (uint32_t)off, (uint8_t *)t->shadow + off, (uint32_t)((size_t)w * bpp));
            continue;
        }
        memcpy((uint8_t *)t->framebuffer_address + off, (uint8_t *)t->shadow + off, (size_t)w * bpp);
    }
}

/* One glyph into the drawing surface, without publishing it. */
static void paint_glyph(struct kestrel_tty *t, unsigned char c, int x, int y, uint32_t fg_color, uint32_t bg_color)
{
    if (t->graphics) return;
    const unsigned char *glyph = kestrel_font[c];
    const uint32_t fg = native_color(t, fg_color);
    const uint32_t bg = native_color(t, bg_color);

    for (int row = 0; row < KFONT_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= t->native_height) continue;
        uint8_t *line = row_ptr(t, py);
        unsigned char bits = glyph[row];
        for (int col = 0; col < KFONT_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= t->native_width) continue;
            put_px(t, line, px, (bits & (0x80u >> col)) ? fg : bg);    /* bit 7 = leftmost pixel */
        }
    }
}

/* Inside a batch (a chunk of tty_write output, see batch_end()) changed
 * rectangles are only collected, and their bounding box goes to VRAM once at
 * the end. VRAM is slow to write even when write-combining. */
static void publish(struct kestrel_tty *t, int x, int y, int w, int h)
{
    if (!t->batch) { publish_now(t, x, y, w, h); return; }
    if (w <= 0 || h <= 0) return;
    if (t->dirty_x1 <= t->dirty_x0) {
        t->dirty_x0 = x; t->dirty_y0 = y; t->dirty_x1 = x + w; t->dirty_y1 = y + h;
        return;
    }
    if (x < t->dirty_x0) t->dirty_x0 = x;
    if (y < t->dirty_y0) t->dirty_y0 = y;
    if (x + w > t->dirty_x1) t->dirty_x1 = x + w;
    if (y + h > t->dirty_y1) t->dirty_y1 = y + h;
}

void draw_char(struct kestrel_tty *t, char c, int x, int y, uint32_t fg_color, uint32_t bg_color)
{
    if (!t || !t->active || t->graphics) return;
    paint_glyph(t, (unsigned char)c, x, y, fg_color, bg_color);    /* char may be signed */
    publish(t, x, y, KFONT_WIDTH, KFONT_HEIGHT);
}

/* ======================================================================== */
/*  2. cell model                                                              */
/* ======================================================================== */

static inline struct tty_cell *cell_at(const struct kestrel_tty *t, int cx, int cy)
{
    return &t->cells[(size_t)cy * (size_t)t->max_cols + (size_t)cx];
}

static void cell_draw(struct kestrel_tty *t, int cx, int cy, bool inverse)
{
    const struct tty_cell *k = cell_at(t, cx, cy);
    draw_char(t, (char)k->ch, cx * TTY_CELL_W, cy * TTY_CELL_H,
              inverse ? k->bg : k->fg, inverse ? k->fg : k->bg);
}

/* Cells x0..x1-1 of row cy from the model to the pixels, published as one
 * rectangle (or not, for a caller that publishes a larger area). */
static void paint_cells(struct kestrel_tty *t, int cy, int x0, int x1, bool pub)
{
    if (t->batch) { t->row_dirty[cy] = 1; return; }    /* drawn at batch_end() */
    if (t->graphics || x0 >= x1) return;
    for (int cx = x0; cx < x1; cx++) {
        const struct tty_cell *k = cell_at(t, cx, cy);
        paint_glyph(t, k->ch, cx * TTY_CELL_W, cy * TTY_CELL_H, k->fg, k->bg);
    }
    if (pub) publish(t, x0 * TTY_CELL_W, cy * TTY_CELL_H, (x1 - x0) * TTY_CELL_W, TTY_CELL_H);
}

static const uint32_t ansi[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

/* The colours new text gets: bold brightens ANSI colours 0-7 (and the
 * default grey to white), reverse swaps foreground and background. */
static void cur_colors(const struct kestrel_tty *t, uint32_t *fg, uint32_t *bg)
{
    uint32_t f = t->fg;
    if (t->bold) f = t->fg_low >= 0 ? ansi[t->fg_low + 8] : t->fg == TTY_FG ? 0xFFFFFFu : t->fg;
    *fg = t->reverse ? t->bg : f;
    *bg = t->reverse ? f : t->bg;
}

static void cell_set(struct kestrel_tty *t, int cx, int cy, unsigned char ch)
{
    struct tty_cell *k = cell_at(t, cx, cy);
    k->ch = ch;
    cur_colors(t, &k->fg, &k->bg);
    if (t->batch) t->row_dirty[cy] = 1;
    else cell_draw(t, cx, cy, false);
}

/* Blank cells x0..x1-1 of row cy in the current colours (the Linux console
 * erases with the current background). */
static void erase_cells(struct kestrel_tty *t, int cy, int x0, int x1, bool pub)
{
    if (x0 < 0) x0 = 0;
    if (x1 > t->max_cols) x1 = t->max_cols;
    uint32_t fg, bg;
    cur_colors(t, &fg, &bg);
    for (int x = x0; x < x1; x++) *cell_at(t, x, cy) = (struct tty_cell){ ' ', fg, bg };
    paint_cells(t, cy, x0, x1, pub);
}

/* ======================================================================== */
/*  3. cursor                                                                  */
/* ======================================================================== */

/* The focused TTY shows a solid inverse block; the others a two-pixel bar at
 * the bottom of the cell, so the monitor that receives typing is obvious.
 * Callers hold the lock (interrupts off). */
static void cursor_paint(struct kestrel_tty *t)
{
    if (t->graphics) return;
    int cx = t->cursor_drawn_col, cy = t->cursor_drawn_row;
    if (t->index == active_keyboard_tty) { cell_draw(t, cx, cy, true); return; }
    cell_draw(t, cx, cy, false);
    uint32_t v = native_color(t, cell_at(t, cx, cy)->fg);
    for (int py = cy * TTY_CELL_H + TTY_CELL_H - 2; py < (cy + 1) * TTY_CELL_H && py < t->native_height; py++)
        for (int px = cx * TTY_CELL_W; px < (cx + 1) * TTY_CELL_W && px < t->native_width; px++)
            put_px(t, row_ptr(t, py), px, v);
    publish(t, cx * TTY_CELL_W, cy * TTY_CELL_H, TTY_CELL_W, TTY_CELL_H);
}

static void cursor_hide(struct kestrel_tty *t)
{
    if (!t->cursor_visible) return;
    cell_draw(t, t->cursor_drawn_col, t->cursor_drawn_row, false);   /* restore the glyph */
    t->cursor_visible = false;
}

static void cursor_show(struct kestrel_tty *t)
{
    if (!t->active || !t->cursor_enabled || t->cursor_hidden || t->cursor_visible) return;
    if (t->cursor_col < 0 || t->cursor_col >= t->max_cols || t->cursor_row < 0 || t->cursor_row >= t->max_rows) return;
    t->cursor_drawn_col = t->cursor_col;
    t->cursor_drawn_row = t->cursor_row;
    cursor_paint(t);
    t->cursor_visible = true;
}

/* After any output: cursor visible at the new position, blink restarted, so
 * typing never shows a lagging or invisible cursor. */
static void cursor_refresh(struct kestrel_tty *t)
{
    cursor_hide(t);
    cursor_show(t);
    t->next_blink = uptime_ms() + TTY_BLINK_MS;
}

/* Timer IRQ (1 kHz): every monitor blinks independently, at its own cell and
 * on its own phase, whatever the shells are doing. A TTY that is being drawn
 * on right now is skipped and retried on the next tick; the others blink. */
void tty_cursor_tick(void)
{
    uint64_t now = uptime_ms();
    for (int i = 0; i < tty_count; i++) {
        struct kestrel_tty *t = &system_ttys[i];
        if (!t->active || t->graphics || !t->cursor_enabled || now < t->next_blink) continue;
        if (!tty_trylock(t)) continue;
        t->next_blink = now + TTY_BLINK_MS;
        if (t->cursor_visible) cursor_hide(t); else cursor_show(t);
        tty_unlock(t);
    }
}

void tty_cursor_enable(struct kestrel_tty *t, bool on)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    t->cursor_enabled = on;
    if (on) cursor_refresh(t); else cursor_hide(t);
    tty_unlock(t);
}

/* ======================================================================== */
/*  4. text layout                                                             */
/* ======================================================================== */

/* Scroll rows top..bot of the scroll region by n rows: n > 0 moves the text
 * up (new blank rows at the bottom), n < 0 down. Pixels move with the cells;
 * each scan line is copied separately, so the copy never leaves this
 * monitor's frame buffer and source and destination never overlap. */
static void scroll_region(struct kestrel_tty *t, int top, int bot, int n)
{
    int h = bot - top + 1, a = n > 0 ? n : -n;
    if (!n || h <= 0) return;
    if (a > h) a = h;
    if (t->batch) {                                     /* cells now, pixels at batch_end() */
        const size_t cells = sizeof(struct tty_cell) * (size_t)t->max_cols;
        if (n > 0 && top == 0 && bot == t->max_rows - 1 && !t->repaint_all && t->pending_up + a < t->max_rows) {
            t->pending_up += a;                         /* one pixel move for all of them */
            memmove(t->row_dirty, t->row_dirty + a, (size_t)(h - a));
        } else {
            t->repaint_all = true;
        }
        if (n > 0) {
            memmove(cell_at(t, 0, top), cell_at(t, 0, top + a), cells * (size_t)(h - a));
            for (int y = bot - a + 1; y <= bot; y++) erase_cells(t, y, 0, t->max_cols, false);
        } else {
            memmove(cell_at(t, 0, top + a), cell_at(t, 0, top), cells * (size_t)(h - a));
            for (int y = top; y < top + a; y++) erase_cells(t, y, 0, t->max_cols, false);
        }
        return;
    }
    if (t->cursor_visible) cursor_hide(t);              /* its pixels move too */
    const size_t row_bytes = (size_t)t->max_cols * TTY_CELL_W * (size_t)t->bytes_pp;
    const size_t cells = sizeof(struct tty_cell) * (size_t)t->max_cols;
    if (n > 0) {
        memmove(cell_at(t, 0, top), cell_at(t, 0, top + a), cells * (size_t)(h - a));
        if (!t->graphics)
            for (int py = top * TTY_CELL_H; py < (bot + 1 - a) * TTY_CELL_H; py++)
                memcpy(row_ptr(t, py), row_ptr(t, py + a * TTY_CELL_H), row_bytes);
        for (int y = bot - a + 1; y <= bot; y++) erase_cells(t, y, 0, t->max_cols, false);
    } else {
        memmove(cell_at(t, 0, top + a), cell_at(t, 0, top), cells * (size_t)(h - a));
        if (!t->graphics)
            for (int py = (bot + 1) * TTY_CELL_H - 1; py >= (top + a) * TTY_CELL_H; py--)
                memcpy(row_ptr(t, py), row_ptr(t, py - a * TTY_CELL_H), row_bytes);
        for (int y = top; y < top + a; y++) erase_cells(t, y, 0, t->max_cols, false);
    }
    if (!t->graphics) publish(t, 0, top * TTY_CELL_H, t->max_cols * TTY_CELL_W, h * TTY_CELL_H);
}

void scroll_screen(struct kestrel_tty *t)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    scroll_region(t, 0, t->max_rows - 1, 1);
    t->cursor_row = t->max_rows - 1;
    t->wrap_pending = false;
    cursor_refresh(t);
    tty_unlock(t);
}

/* Line feed: down one row, scrolling the region at its bottom margin. */
static void index_down(struct kestrel_tty *t)
{
    t->wrap_pending = false;
    if (t->cursor_row == t->scroll_bot) scroll_region(t, t->scroll_top, t->scroll_bot, 1);
    else if (t->cursor_row < t->max_rows - 1) t->cursor_row++;
}

static void reverse_index(struct kestrel_tty *t)
{
    t->wrap_pending = false;
    if (t->cursor_row == t->scroll_top) scroll_region(t, t->scroll_top, t->scroll_bot, -1);
    else if (t->cursor_row > 0) t->cursor_row--;
}

static void newline(struct kestrel_tty *t)
{
    t->cursor_col = 0;
    index_down(t);
}

static void put_glyph(struct kestrel_tty *t, unsigned char ch)
{
    if (t->wrap_pending) newline(t);                    /* deferred wrap, as in xterm */
    cell_set(t, t->cursor_col, t->cursor_row, ch);
    if (t->cursor_col + 1 < t->max_cols) t->cursor_col++;
    else t->wrap_pending = true;
}

static void move_to(struct kestrel_tty *t, int col, int row)
{
    t->cursor_col = col < 0 ? 0 : col >= t->max_cols ? t->max_cols - 1 : col;
    t->cursor_row = row < 0 ? 0 : row >= t->max_rows ? t->max_rows - 1 : row;
    t->wrap_pending = false;
}

/* Batches: text output only updates the cell model, marks rows dirty and
 * counts full-screen scrolls. batch_end() then moves the pixels once by all
 * the scrolled rows, draws the dirty rows and publishes the result in one
 * go, so a burst that scrolls the screen a thousand times costs about one
 * screen redraw instead of a thousand. */
static void batch_begin(struct kestrel_tty *t)
{
    t->batch = true;
    t->dirty_x0 = t->dirty_y0 = t->dirty_x1 = t->dirty_y1 = 0;
}

static void batch_end(struct kestrel_tty *t)
{
    t->batch = false;
    const int rows = t->max_rows, text_h = rows * TTY_CELL_H;
    int lo = rows, hi = -1;
    if (t->repaint_all) {
        lo = 0; hi = rows - 1;
        for (int y = 0; y < rows; y++) paint_cells(t, y, 0, t->max_cols, false);
    } else {
        if (t->pending_up && !t->graphics) {
            const size_t row_bytes = (size_t)t->max_cols * TTY_CELL_W * (size_t)t->bytes_pp;
            for (int py = 0; py < text_h - t->pending_up * TTY_CELL_H; py++)
                memcpy(row_ptr(t, py), row_ptr(t, py + t->pending_up * TTY_CELL_H), row_bytes);
        }
        if (t->pending_up) { lo = 0; hi = rows - 1; }
        for (int y = 0; y < rows; y++)
            if (t->row_dirty[y]) {
                paint_cells(t, y, 0, t->max_cols, false);
                if (y < lo) lo = y;
                if (y > hi) hi = y;
            }
    }
    memset(t->row_dirty, 0, (size_t)rows);
    t->pending_up = 0;
    t->repaint_all = false;
    if (t->graphics) return;
    if (hi >= lo) publish(t, 0, lo * TTY_CELL_H, t->max_cols * TTY_CELL_W, (hi - lo + 1) * TTY_CELL_H);
    if (t->dirty_x1 > t->dirty_x0)
        publish_now(t, t->dirty_x0, t->dirty_y0, t->dirty_x1 - t->dirty_x0, t->dirty_y1 - t->dirty_y0);
}

static void clear_locked(struct kestrel_tty *t)
{
    if (!t->graphics) {
        for (int py = 0; py < t->native_height; py++)   /* the whole monitor, not just the text */
            memset(row_ptr(t, py), 0x00, (size_t)t->native_width * (size_t)t->bytes_pp);
        publish(t, 0, 0, t->native_width, t->native_height);
    }
    for (int y = 0; y < t->max_rows; y++)
        for (int x = 0; x < t->max_cols; x++) *cell_at(t, x, y) = (struct tty_cell){ ' ', t->fg, TTY_BG };
    if (t->batch) t->repaint_all = true;                /* the pixels were just wiped */
    t->cursor_col = t->cursor_row = 0;
    t->wrap_pending = false;
    t->cursor_visible = false;
}

void tty_clear(struct kestrel_tty *t)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    clear_locked(t);
    cursor_refresh(t);
    tty_unlock(t);
}

/* ---- escape sequences ------------------------------------------------------ */
enum { ES_NONE, ES_ESC, ES_CSI, ES_OSC, ES_OSC_ESC, ES_CHARSET };

static void reset_locked(struct kestrel_tty *t, bool clear)
{
    t->fg = TTY_FG; t->bg = TTY_BG; t->fg_low = -1;
    t->bold = t->reverse = false;
    t->scroll_top = 0; t->scroll_bot = t->max_rows - 1;
    t->esc_state = ES_NONE;
    t->utf_left = t->utf_n = 0;
    t->cursor_hidden = false;
    t->app_cursor = false;
    t->wrap_pending = false;
    t->saved.col = t->saved.row = 0;
    t->saved.fg = TTY_FG; t->saved.bg = TTY_BG; t->saved.fg_low = -1;
    t->saved.bold = t->saved.reverse = false;
    if (clear) clear_locked(t);
}

void tty_reset(struct kestrel_tty *t, bool clear)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    reset_locked(t, clear);
    cursor_refresh(t);
    tty_unlock(t);
}

static void save_cursor(struct kestrel_tty *t)
{
    t->saved.col = t->cursor_col; t->saved.row = t->cursor_row;
    t->saved.fg = t->fg; t->saved.bg = t->bg; t->saved.fg_low = t->fg_low;
    t->saved.bold = t->bold; t->saved.reverse = t->reverse;
}

static void restore_cursor(struct kestrel_tty *t)
{
    move_to(t, t->saved.col, t->saved.row);
    t->fg = t->saved.fg; t->bg = t->saved.bg; t->fg_low = t->saved.fg_low;
    t->bold = t->saved.bold; t->reverse = t->saved.reverse;
}

static void reply(struct kestrel_tty *t, const char *fmt, ...)
{
    if (!t->reply) return;
    char buf[32];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && n < (int)sizeof buf) t->reply(t->reply_ctx, buf, (size_t)n);
}

/* xterm's 256-colour palette: the 16 ANSI colours, a 6x6x6 cube, 24 greys. */
static uint32_t color256(int i)
{
    if (i < 16) return ansi[i];
    if (i < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return (uint32_t)lv[i / 36] << 16 | (uint32_t)lv[i / 6 % 6] << 8 | lv[i % 6];
    }
    uint32_t g = (uint32_t)(8 + 10 * (i - 232));
    return g << 16 | g << 8 | g;
}

/* CSI parameter i: missing or 0 gives `def`. */
static int arg(const struct kestrel_tty *t, int i, int def)
{
    int v = i < t->esc_n ? t->esc_args[i] : -1;
    return v <= 0 ? def : v;
}

static void sgr(struct kestrel_tty *t)
{
    int n = t->esc_n ? t->esc_n : 1;
    for (int i = 0; i < n; i++) {
        int v = i < t->esc_n && t->esc_args[i] > 0 ? t->esc_args[i] : 0;
        if ((v == 38 || v == 48) && i + 1 < t->esc_n) {          /* 38;5;n and 38;2;r;g;b */
            uint32_t c;
            if (t->esc_args[i + 1] == 5 && i + 2 < t->esc_n) {
                c = color256(t->esc_args[i + 2] & 255);
                i += 2;
            } else if (t->esc_args[i + 1] == 2 && i + 4 < t->esc_n) {
                c = (uint32_t)(t->esc_args[i + 2] & 255) << 16 | (uint32_t)(t->esc_args[i + 3] & 255) << 8 |
                    (uint32_t)(t->esc_args[i + 4] & 255);
                i += 4;
            } else {
                break;
            }
            if (v == 38) { t->fg = c; t->fg_low = -1; } else t->bg = c;
            continue;
        }
        if (v == 0)                    { t->fg = TTY_FG; t->bg = TTY_BG; t->fg_low = -1; t->bold = t->reverse = false; }
        else if (v == 1)               t->bold = true;
        else if (v == 22)              t->bold = false;
        else if (v == 7)               t->reverse = true;
        else if (v == 27)              t->reverse = false;
        else if (v >= 30 && v <= 37)   { t->fg = ansi[v - 30]; t->fg_low = v - 30; }
        else if (v >= 90 && v <= 97)   { t->fg = ansi[v - 90 + 8]; t->fg_low = -1; }
        else if (v >= 40 && v <= 47)   t->bg = ansi[v - 40];
        else if (v >= 100 && v <= 107) t->bg = ansi[v - 100 + 8];
        else if (v == 39)              { t->fg = TTY_FG; t->fg_low = -1; }
        else if (v == 49)              t->bg = TTY_BG;
        /* 2 dim, 3 italic, 4 underline, 5 blink, 8 hidden...: not shown */
    }
}

/* Insert (n > 0) or delete (n < 0) |n| characters at the cursor; the rest
 * of the row shifts, blanks fill in at the right edge or the cursor. */
static void shift_row(struct kestrel_tty *t, int n)
{
    int col = t->cursor_col, row = t->cursor_row, w = t->max_cols - col, a = n > 0 ? n : -n;
    if (a > w) a = w;
    struct tty_cell *r = cell_at(t, 0, row);
    if (n > 0) {
        memmove(r + col + a, r + col, sizeof *r * (size_t)(w - a));
        erase_cells(t, row, col, col + a, false);
    } else {
        memmove(r + col, r + col + a, sizeof *r * (size_t)(w - a));
        erase_cells(t, row, t->max_cols - a, t->max_cols, false);
    }
    paint_cells(t, row, col, t->max_cols, true);
}

static void csi_dispatch(struct kestrel_tty *t, char ch, bool vt)
{
    int n1 = arg(t, 0, 1), row = t->cursor_row, col = t->cursor_col;
    if (t->esc_priv == '?') {                           /* DEC private modes */
        if (ch != 'h' && ch != 'l') return;
        for (int i = 0; i < t->esc_n; i++) {
            if (t->esc_args[i] == 25) t->cursor_hidden = ch == 'l';
            else if (t->esc_args[i] == 1) t->app_cursor = ch == 'h';
        }
        return;
    }
    if (t->esc_priv) return;                            /* ESC[>c and friends */
    switch (ch) {
    case 'A': move_to(t, col, row - n1); break;
    case 'B': case 'e': move_to(t, col, row + n1); break;
    case 'C': case 'a': move_to(t, col + n1, row); break;
    case 'D': move_to(t, col - n1, row); break;
    case 'E': move_to(t, 0, row + n1); break;
    case 'F': move_to(t, 0, row - n1); break;
    case 'G': case '`': move_to(t, n1 - 1, row); break;
    case 'd': move_to(t, col, n1 - 1); break;
    case 'H': case 'f': move_to(t, arg(t, 1, 1) - 1, n1 - 1); break;
    case 'J': {
        int m = arg(t, 0, 0);
        if (m == 2 && !vt) { clear_locked(t); break; }  /* kernel writers: clear and home */
        if (m == 0) {
            erase_cells(t, row, col, t->max_cols, true);
            for (int y = row + 1; y < t->max_rows; y++) erase_cells(t, y, 0, t->max_cols, true);
        } else if (m == 1) {
            for (int y = 0; y < row; y++) erase_cells(t, y, 0, t->max_cols, true);
            erase_cells(t, row, 0, col + 1, true);
        } else {
            for (int y = 0; y < t->max_rows; y++) erase_cells(t, y, 0, t->max_cols, true);
        }
        break;
    }
    case 'K': {
        int m = arg(t, 0, 0);
        erase_cells(t, row, m == 0 ? col : 0, m == 1 ? col + 1 : t->max_cols, true);
        break;
    }
    case 'L': if (row >= t->scroll_top && row <= t->scroll_bot) scroll_region(t, row, t->scroll_bot, -n1); break;
    case 'M': if (row >= t->scroll_top && row <= t->scroll_bot) scroll_region(t, row, t->scroll_bot, n1); break;
    case '@': shift_row(t, n1); break;
    case 'P': shift_row(t, -n1); break;
    case 'X': erase_cells(t, row, col, col + n1, true); break;
    case 'm': sgr(t); break;
    case 'n':
        if (arg(t, 0, 0) == 5) reply(t, "\033[0n");
        else if (arg(t, 0, 0) == 6) reply(t, "\033[%d;%dR", row + 1, col + 1);
        break;
    case 'c': reply(t, "\033[?6c"); break;               /* "VT102", like the Linux console */
    case 'r': {
        int top = n1 - 1, bot = arg(t, 1, t->max_rows) - 1;
        if (bot >= t->max_rows) bot = t->max_rows - 1;
        if (top < bot) { t->scroll_top = top; t->scroll_bot = bot; move_to(t, 0, 0); }
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    default: break;                                     /* modes, LEDs, ...: ignored */
    }
}

static void esc_dispatch(struct kestrel_tty *t, char ch)
{
    t->esc_state = ES_NONE;
    switch (ch) {
    case '[': t->esc_state = ES_CSI; t->esc_n = 0; t->esc_val = -1; t->esc_priv = 0; break;
    case ']': t->esc_state = ES_OSC; break;             /* window title etc.: skipped */
    case '(': case ')': case '*': case '+': t->esc_state = ES_CHARSET; break;
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'c': reset_locked(t, true); break;
    case 'D': index_down(t); break;
    case 'E': newline(t); break;
    case 'M': reverse_index(t); break;
    default: break;                                     /* ESC = / ESC > (keypad) and others */
    }
}

static void csi_char(struct kestrel_tty *t, char ch, bool vt)
{
    if (ch >= '0' && ch <= '9') {
        if (t->esc_val < 0) t->esc_val = 0;
        if (t->esc_val < 100000) t->esc_val = t->esc_val * 10 + (ch - '0');
        return;
    }
    if (ch == ';' || ch == ':') {
        if (t->esc_n < 16) t->esc_args[t->esc_n++] = t->esc_val;
        t->esc_val = -1;
        return;
    }
    if (ch >= 0x3C && ch <= 0x3F) { if (!t->esc_n && t->esc_val < 0) t->esc_priv = ch; return; }
    if (ch >= 0x20 && ch <= 0x2F) return;               /* intermediate bytes: ignored */
    if (t->esc_n < 16 && (t->esc_val >= 0 || t->esc_n)) t->esc_args[t->esc_n++] = t->esc_val;
    t->esc_state = ES_NONE;
    csi_dispatch(t, ch, vt);
}

/* ---- characters -------------------------------------------------------------- */
/* Code point -> glyph of the Windows-1252 font. */
static unsigned char unicode_glyph(uint32_t cp)
{
    static const uint16_t w1252[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
    };
    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF)) return (unsigned char)cp;
    for (int i = 0; i < 32; i++) if (w1252[i] && w1252[i] == cp) return (unsigned char)(0x80 + i);
    /* look-alikes for what the font does not have */
    switch (cp) {
    case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
    case 0x254C: case 0x254D: case 0x2550: case 0x2010: case 0x2011: case 0x2012: case 0x2015: case 0x2212:
        return '-';
    case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B:
    case 0x254E: case 0x254F: case 0x2551:
        return '|';
    case 0x2190: return '<';
    case 0x2192: return '>';
    case 0x2191: return '^';
    case 0x2193: return 'v';
    case 0x25CF: case 0x2219: return 0x95;              /* bullet */
    case 0x2588: case 0x2591: case 0x2592: case 0x2593: return '#';
    }
    if (cp >= 0x2500 && cp <= 0x257F) return '+';       /* corners and junctions */
    return '?';
}

/* A UTF-8 sequence broken off by another byte: show its bytes as they are. */
static void utf_flush(struct kestrel_tty *t)
{
    for (int i = 0; i < t->utf_n; i++) put_glyph(t, t->utf_raw[i]);
    t->utf_n = t->utf_left = 0;
}

static void putc_locked(struct kestrel_tty *t, char c, bool vt)
{
    unsigned char ch = (unsigned char)c;
    if (t->utf_left) {
        if ((ch & 0xC0) == 0x80) {
            t->utf_raw[t->utf_n++] = ch;
            t->utf_cp = t->utf_cp << 6 | (ch & 0x3Fu);
            if (--t->utf_left == 0) { t->utf_n = 0; put_glyph(t, unicode_glyph(t->utf_cp)); }
            return;
        }
        utf_flush(t);
    }
    if (t->esc_state == ES_OSC || t->esc_state == ES_OSC_ESC) {   /* until BEL or ESC \ */
        if (ch == 0x07) { t->esc_state = ES_NONE; return; }
        if (t->esc_state == ES_OSC_ESC) {
            t->esc_state = ES_NONE;
            if (ch != '\\') esc_dispatch(t, c);
            return;
        }
        if (ch == 0x1B) t->esc_state = ES_OSC_ESC;
        return;
    }
    if (ch >= 0x80) {
        t->esc_state = ES_NONE;
        if (ch >= 0xC2 && ch <= 0xF4) {                 /* start of a UTF-8 sequence */
            t->utf_left = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            t->utf_cp = ch & (0x3Fu >> t->utf_left);
            t->utf_raw[0] = ch;
            t->utf_n = 1;
            return;
        }
        put_glyph(t, ch);                               /* not UTF-8: the byte's own glyph */
        return;
    }
    if (ch < 0x20 || ch == 0x7F) {                      /* control characters act even inside a sequence */
        switch (ch) {
        case 0x1B: t->esc_state = ES_ESC; return;
        case 0x18: case 0x1A: t->esc_state = ES_NONE; return;   /* CAN, SUB */
        case '\n': case 0x0B: case 0x0C:
            if (vt) index_down(t); else newline(t);
            return;
        case '\r': t->cursor_col = 0; t->wrap_pending = false; return;
        case '\t':
            t->wrap_pending = false;
            t->cursor_col = (t->cursor_col / 8 + 1) * 8;
            if (t->cursor_col >= t->max_cols) t->cursor_col = t->max_cols - 1;
            return;
        case '\b':
            if (t->wrap_pending) t->wrap_pending = false;
            else if (t->cursor_col > 0) t->cursor_col--;
            else if (!vt && t->cursor_row > 0) { t->cursor_row--; t->cursor_col = t->max_cols - 1; }
            return;
        default: return;                                /* BEL, NUL, SO/SI, DEL, ... */
        }
    }
    switch (t->esc_state) {
    case ES_ESC:     esc_dispatch(t, c); return;
    case ES_CSI:     csi_char(t, c, vt); return;
    case ES_CHARSET: t->esc_state = ES_NONE; return;
    default:         put_glyph(t, ch); return;
    }
}

void tty_putc(struct kestrel_tty *t, char c)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    putc_locked(t, c, false);
    cursor_refresh(t);
    tty_unlock(t);
}

void tty_put_glyph(struct kestrel_tty *t, unsigned char c)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    put_glyph(t, c);
    cursor_refresh(t);
    tty_unlock(t);
}

static void write_common(struct kestrel_tty *t, const char *s, size_t n, bool vt)
{
    if (!t || !t->active || !s) return;
    while (n) {
        size_t chunk = n < 4096 ? n : 4096;     /* let other writers of this TTY in between */
        if (!tty_lock(t)) return;               /* busy, and we cannot wait (IRQ context) */
        cursor_hide(t);
        batch_begin(t);
        for (size_t i = 0; i < chunk; i++) putc_locked(t, s[i], vt);
        batch_end(t);
        cursor_refresh(t);
        tty_unlock(t);
        s += chunk; n -= chunk;
    }
}

void tty_write(struct kestrel_tty *t, const char *s, size_t n) { write_common(t, s, n, false); }
void tty_write_vt(struct kestrel_tty *t, const char *s, size_t n) { write_common(t, s, n, true); }

void tty_puts(struct kestrel_tty *t, const char *s) { if (s) tty_write(t, s, strlen(s)); }

void tty_printf(struct kestrel_tty *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    tty_write(t, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

void tty_set_color(struct kestrel_tty *t, uint32_t fg, uint32_t bg) { if (t) { t->fg = fg; t->bg = bg; t->fg_low = -1; } }

/* Line-editor backspace: move left (wrapping back over a line break) and
 * overwrite that cell with a blank in the background colour. */
void tty_backspace(struct kestrel_tty *t)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    if (t->wrap_pending) t->wrap_pending = false;       /* the last column itself */
    else if (t->cursor_col > 0) t->cursor_col--;
    else if (t->cursor_row > 0) { t->cursor_row--; t->cursor_col = t->max_cols - 1; }
    cell_set(t, t->cursor_col, t->cursor_row, ' ');
    cursor_refresh(t);
    tty_unlock(t);
}

void tty_write_current(const char *s, size_t n) { tty_write(tty_current(), s, n); }

/* ======================================================================== */
/*  5. input                                                                   */
/* ======================================================================== */

bool tty_input_active(void) { return input_active; }

void tty_set_focus(int index)
{
    if (index < 0 || index >= tty_count || !system_ttys[index].active) return;
    uint64_t f = irq_save();                    /* also called from the keyboard ISR */
    int old = active_keyboard_tty;
    active_keyboard_tty = index;
    irq_restore(f);
    if (old == index) return;
    /* Repaint both cursors in their new style (block <-> bar). A TTY that is
     * busy repaints its cursor itself when the current operation ends. */
    struct kestrel_tty *ttys[2] = { &system_ttys[old], &system_ttys[index] };
    for (int i = 0; i < 2; i++)
        if (ttys[i]->active && tty_trylock(ttys[i])) { cursor_refresh(ttys[i]); tty_unlock(ttys[i]); }
    kprintf("tty: keyboard focus -> tty%d (monitor %d, %dx%d)\n", index + 1, index,
            ttys[1]->native_width, ttys[1]->native_height);
}

/* Keyboard ISR: Ctrl+Alt+F1..F12 selects TTY 0..11. The combination is
 * always consumed, even without a TTY behind that key, so it never reaches
 * a shell as a stray function key. */
bool tty_hotkey(uint16_t key, uint8_t mods)
{
    if (!input_active || key < KEY_F1 || key > KEY_F12) return false;
    if ((mods & (MOD_CTRL | MOD_ALT)) != (MOD_CTRL | MOD_ALT)) return false;
    tty_set_focus((int)(key - KEY_F1));
    return true;
}

/* Keyboard ISR: queue a key on the TTY that has the focus and wake the task
 * reading that TTY. A full queue drops the key, like the hardware buffer. */
bool tty_route_key(const struct key_event *ev)
{
    if (!input_active) return false;
    struct kestrel_tty *t = &system_ttys[active_keyboard_tty];
    uint64_t f = irq_save();                    /* USB injects from the timer hook */
    uint32_t next = (t->keyq_head + 1) % TTY_KEYQ;
    if (next != t->keyq_tail) {
        t->keyq[t->keyq_head] = *ev;
        t->keyq_head = next;
        wakeup(t->keyq);
    }
    irq_restore(f);
    return true;
}

bool tty_read_key(struct kestrel_tty *t, struct key_event *ev, bool block)
{
    for (;;) {
        uint64_t f = irq_save();
        if (t->keyq_head != t->keyq_tail) {
            *ev = t->keyq[t->keyq_tail];
            t->keyq_tail = (t->keyq_tail + 1) % TTY_KEYQ;
            irq_restore(f);
            return true;
        }
        if (!block) { irq_restore(f); return false; }
        if (signal_pending()) { irq_restore(f); return false; }    /* user reader: EINTR */
        sleep_on(t->keyq);                      /* returns with interrupts disabled */
        irq_restore(f);
    }
}

bool tty_has_key(struct kestrel_tty *t) { return t->keyq_head != t->keyq_tail; }

void tty_flush_keys(struct kestrel_tty *t)
{
    uint64_t f = irq_save();
    t->keyq_tail = t->keyq_head;
    irq_restore(f);
}

/* Canonical-mode line discipline for /dev/tty: edit one line in this TTY's
 * input_buffer (echo, Backspace, Ctrl+U/C/L, Ctrl+D = EOF on an empty line),
 * then return it in as many read() calls as the reader needs. */
long tty_read_line(struct kestrel_tty *t, char *buf, size_t n)
{
    if (!t->line_ready) {
        t->buffer_index = 0;
        for (;;) {
            struct key_event ev;
            if (!tty_read_key(t, &ev, true)) return -ERESTARTSYS;  /* signal */
            char c = ev.ascii;
            if (c == '\n') { t->input_buffer[t->buffer_index++] = '\n'; tty_putc(t, '\n'); break; }
            if (c == '\b') { if (t->buffer_index) { t->buffer_index--; tty_backspace(t); } continue; }
            if (c == 3)    { tty_puts(t, "^C\n"); t->buffer_index = 0; t->input_buffer[t->buffer_index++] = '\n'; break; }
            if (c == 4)    { if (t->buffer_index == 0) return 0; continue; }
            if (c == 12)   { tty_clear(t); tty_write(t, t->input_buffer, (size_t)t->buffer_index); continue; }
            if (c == 21)   { while (t->buffer_index) { t->buffer_index--; tty_backspace(t); } continue; }
            if (c >= ' ' && c < 127 && t->buffer_index < TTY_INPUT_MAX - 1) {
                t->input_buffer[t->buffer_index++] = c;
                tty_putc(t, c);
            }
        }
        t->line_len = t->buffer_index;
        t->line_pos = 0;
        t->line_ready = true;
    }
    size_t avail = (size_t)(t->line_len - t->line_pos);
    if (n > avail) n = avail;
    memcpy(buf, t->input_buffer + t->line_pos, n);
    t->line_pos += (int)n;
    if (t->line_pos >= t->line_len) { t->line_ready = false; t->buffer_index = 0; }
    return (long)n;
}

/* ======================================================================== */
/*  set-up and lookup                                                          */
/* ======================================================================== */

bool tty_ready(void) { return tty_count > 0; }

struct kestrel_tty *tty_get(int index)
{
    return index >= 0 && index < tty_count ? &system_ttys[index] : NULL;
}

struct kestrel_tty *tty_current(void)
{
    struct tcb *cur = current_task();
    struct kestrel_tty *t = tty_get(cur ? cur->tty : 0);
    return t ? t : tty_get(0);
}

void tty_bind_current(int index)
{
    struct tcb *cur = current_task();
    if (cur && tty_get(index)) cur->tty = index;
}

static bool tty_setup(struct kestrel_tty *t, int index, const struct display_head *h)
{
    memset(t, 0, sizeof *t);
    t->framebuffer_address = h->vram;
    t->shadow        = h->shadow;
    t->native_width  = h->width;
    t->native_height = h->height;
    t->pitch         = h->pitch;
    t->write_span    = h->write_span;
    t->write_ctx     = h->write_ctx;
    t->bytes_pp      = h->bytes_pp;
    t->r_pos = h->r_pos; t->g_pos = h->g_pos; t->b_pos = h->b_pos;
    t->index    = index;
    t->max_cols = h->width / TTY_CELL_W;
    t->max_rows = h->height / TTY_CELL_H;
    if (t->max_cols < 1 || t->max_rows < 1) return false;
    t->cells = kmalloc(sizeof(struct tty_cell) * (size_t)t->max_cols * (size_t)t->max_rows);
    t->row_dirty = kzalloc((size_t)t->max_rows);
    if (!t->cells || !t->row_dirty) return false;
    t->fg = TTY_FG;
    t->bg = TTY_BG;
    t->fg_low = -1;
    t->scroll_bot = t->max_rows - 1;
    t->saved.fg = TTY_FG; t->saved.bg = TTY_BG; t->saved.fg_low = -1;
    t->cursor_enabled = true;
    t->active = true;
    return true;
}

int tty_init_all(void)
{
    if (tty_count) return tty_count;
    int n = 0;
    for (int i = 0; i < display_count() && i < MAX_MONITORS; i++) {
        if (!tty_setup(&system_ttys[i], i, display_get(i))) {
            kprintf("tty: no terminal for monitor %d (out of memory or too small)\n", i);
            break;                              /* TTY numbers must match monitor numbers */
        }
        n++;
    }
    tty_count = n;
    for (int i = 0; i < n; i++) {
        struct kestrel_tty *t = &system_ttys[i];
        tty_clear(t);
        kprintf("tty: tty%d on monitor %d: %dx%d pixels, %d x %d cells%s\n", i + 1, i,
                t->native_width, t->native_height, t->max_cols, t->max_rows,
                t->shadow ? ", drawn through a RAM shadow" : "");
    }
    if (!n) return 0;
    active_keyboard_tty = 0;
    pit_add_tick_hook(tty_cursor_tick);
    input_active = true;
    return n;
}

/* ======================================================================== */
/*  graphics clients (/dev/fbN, the X server)                                 */
/* ======================================================================== */
int tty_set_graphics(int index, bool on)
{
    struct kestrel_tty *t = tty_get(index);
    if (!t || !t->active) return -ENODEV;
    if (t->bytes_pp != 4) return -EINVAL;               /* 32-bpp monitors only */
    if (!tty_lock(t)) return -EBUSY;
    if (on && !t->graphics) {
        cursor_hide(t);
        t->graphics = true;
    } else if (!on && t->graphics) {
        t->graphics = false;                            /* repaint the text */
        for (int py = 0; py < t->native_height; py++)
            memset(row_ptr(t, py), 0x00, (size_t)t->native_width * (size_t)t->bytes_pp);
        for (int cy = 0; cy < t->max_rows; cy++)
            for (int cx = 0; cx < t->max_cols; cx++) {
                const struct tty_cell *k = cell_at(t, cx, cy);
                const unsigned char *glyph = kestrel_font[k->ch];
                const uint32_t fg = native_color(t, k->fg), bg = native_color(t, k->bg);
                for (int row = 0; row < KFONT_HEIGHT; row++) {
                    uint8_t *line = row_ptr(t, cy * TTY_CELL_H + row);
                    for (int col = 0; col < KFONT_WIDTH; col++)
                        put_px(t, line, cx * TTY_CELL_W + col, (glyph[row] & (0x80u >> col)) ? fg : bg);
                }
            }
        publish(t, 0, 0, t->native_width, t->native_height);
        t->cursor_visible = false;
        cursor_refresh(t);
    }
    tty_unlock(t);
    return 0;
}

/* Like tty_blit(), from a user buffer. Its pages are faulted in first (with
 * no lock held); the copy under the TTY lock then uses no-fault user
 * copies, and a page that went away in between makes the blit fail with
 * -EFAULT instead of touching a bad address. */
int tty_blit_user(int index, int x, int y, int w, int h, uint64_t usrc, uint32_t pitch)
{
    struct kestrel_tty *t = tty_get(index);
    if (!t || !t->active || t->bytes_pp != 4) return -ENODEV;
    if (w <= 0 || h <= 0) return 0;
    uint64_t span = (uint64_t)(h - 1) * pitch + (uint64_t)w * 4;
    if (!user_range_ok(usrc, span)) return -EFAULT;
    for (uint64_t a = usrc & ~0xFFFull; a < usrc + span; a += 4096) {
        uint32_t probe;
        if (get_user_u32(&probe, (const void *)(a < usrc ? usrc : a))) return -EFAULT;
    }
    if (x < 0) { usrc -= (uint64_t)x * 4; w += x; x = 0; }
    if (y < 0) { usrc -= (uint64_t)(int64_t)y * pitch; h += y; y = 0; }
    if (x + w > t->native_width)  w = t->native_width - x;
    if (y + h > t->native_height) h = t->native_height - y;
    if (w <= 0 || h <= 0) return 0;
    if (!tty_lock(t)) return -EBUSY;
    int rc = 0;
    for (int py = 0; py < h && !rc; py++)
        rc = copy_from_user_nofault(row_ptr(t, y + py) + (size_t)x * 4, (const void *)(usrc + (uint64_t)py * pitch),
                                    (size_t)w * 4);
    publish(t, x, y, w, h);
    tty_unlock(t);
    return rc;
}

int tty_blit(int index, int x, int y, int w, int h, const void *src, uint32_t pitch)
{
    struct kestrel_tty *t = tty_get(index);
    if (!t || !t->active || t->bytes_pp != 4) return -ENODEV;
    const uint8_t *s = src;
    if (x < 0) { s -= x * 4; w += x; x = 0; }
    if (y < 0) { s -= (int64_t)y * pitch; h += y; y = 0; }
    if (x + w > t->native_width)  w = t->native_width - x;
    if (y + h > t->native_height) h = t->native_height - y;
    if (w <= 0 || h <= 0) return 0;
    if (!tty_lock(t)) return -EBUSY;
    for (int py = 0; py < h; py++)
        memcpy(row_ptr(t, y + py) + (size_t)x * 4, s + (size_t)py * pitch, (size_t)w * 4);
    publish(t, x, y, w, h);
    tty_unlock(t);
    return 0;
}
