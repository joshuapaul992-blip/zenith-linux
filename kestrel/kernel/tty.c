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
static void publish(const struct kestrel_tty *t, int x, int y, int w, int h)
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

void draw_char(struct kestrel_tty *t, char c, int x, int y, uint32_t fg_color, uint32_t bg_color)
{
    if (!t || !t->active || t->graphics) return;
    const unsigned char *glyph = kestrel_font[(unsigned char)c];   /* char may be signed */
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

static void cell_set(struct kestrel_tty *t, int cx, int cy, unsigned char ch)
{
    *cell_at(t, cx, cy) = (struct tty_cell){ ch, t->fg, t->bg };
    cell_draw(t, cx, cy, false);
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
    if (!t->active || !t->cursor_enabled || t->cursor_visible) return;
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

static void blank_row(struct kestrel_tty *t, int cy)
{
    for (int x = 0; x < t->max_cols; x++) *cell_at(t, x, cy) = (struct tty_cell){ ' ', t->fg, TTY_BG };
}

static void scroll_locked(struct kestrel_tty *t)
{
    const size_t row_bytes = (size_t)t->max_cols * TTY_CELL_W * (size_t)t->bytes_pp;
    const int text_h = t->max_rows * TTY_CELL_H;

    if (t->graphics) goto cells;                /* a graphics client owns the pixels */
    /* Shift pixel rows 16..text_h-1 of this monitor up to 0..text_h-17. One
     * scan line per copy: source and destination never overlap, so memcpy is
     * valid, and with this TTY's own pitch and text size the copy never
     * leaves this monitor's frame buffer. */
    for (int py = 0; py < text_h - TTY_CELL_H; py++)
        memcpy(row_ptr(t, py), row_ptr(t, py + TTY_CELL_H), row_bytes);
    for (int py = text_h - TTY_CELL_H; py < text_h; py++)        /* bottom text row: black */
        memset(row_ptr(t, py), 0x00, row_bytes);

cells:
    memmove(t->cells, t->cells + t->max_cols, sizeof(struct tty_cell) * (size_t)t->max_cols * (size_t)(t->max_rows - 1));
    blank_row(t, t->max_rows - 1);

    if (t->cursor_visible && t->cursor_drawn_row > 0) t->cursor_drawn_row--;   /* moved with the pixels */
    else t->cursor_visible = false;
    t->cursor_row = t->max_rows - 1;
    if (!t->graphics) publish(t, 0, 0, t->max_cols * TTY_CELL_W, text_h);
}

void scroll_screen(struct kestrel_tty *t)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    scroll_locked(t);
    cursor_refresh(t);
    tty_unlock(t);
}

static void newline(struct kestrel_tty *t)
{
    t->cursor_col = 0;
    if (++t->cursor_row >= t->max_rows) scroll_locked(t);
}

static void put_glyph(struct kestrel_tty *t, unsigned char ch)
{
    cell_set(t, t->cursor_col, t->cursor_row, ch);
    if (++t->cursor_col >= t->max_cols) newline(t);    /* wrap to the next row */
}

static void clear_locked(struct kestrel_tty *t)
{
    if (!t->graphics) {
        for (int py = 0; py < t->native_height; py++)   /* the whole monitor, not just the text */
            memset(row_ptr(t, py), 0x00, (size_t)t->native_width * (size_t)t->bytes_pp);
        publish(t, 0, 0, t->native_width, t->native_height);
    }
    for (int y = 0; y < t->max_rows; y++) blank_row(t, y);
    t->cursor_col = t->cursor_row = 0;
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

/* ANSI SGR subset: 0 reset, 1 bold (bright), 30-37/90-97 fg, 40-47/100-107
 * bg, 39/49 defaults; also ESC[2J (clear) and ESC[H (home). */
static const uint32_t ansi[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static void apply_sgr(struct kestrel_tty *t, int v)
{
    if (v == 0)                    { t->fg = TTY_FG; t->bg = TTY_BG; }
    else if (v == 1)               t->fg = t->fg == TTY_FG ? 0xFFFFFF : t->fg;
    else if (v >= 30 && v <= 37)   t->fg = ansi[v - 30];
    else if (v >= 90 && v <= 97)   t->fg = ansi[v - 90 + 8];
    else if (v >= 40 && v <= 47)   t->bg = ansi[v - 40];
    else if (v >= 100 && v <= 107) t->bg = ansi[v - 100 + 8];
    else if (v == 39)              t->fg = TTY_FG;
    else if (v == 49)              t->bg = TTY_BG;
}

static void escape(struct kestrel_tty *t, char ch)
{
    if (t->esc_state == 1) {
        if (ch == '[') { t->esc_state = 2; t->esc_n = 0; t->esc_val = -1; return; }
        t->esc_state = 0;
        return;
    }
    if (ch >= '0' && ch <= '9') { t->esc_val = (t->esc_val < 0 ? 0 : t->esc_val) * 10 + (ch - '0'); return; }
    if (ch == ';') { if (t->esc_n < 8) t->esc_args[t->esc_n++] = t->esc_val < 0 ? 0 : t->esc_val; t->esc_val = -1; return; }
    if (t->esc_n < 8) t->esc_args[t->esc_n++] = t->esc_val < 0 ? 0 : t->esc_val;
    if (ch == 'm') for (int i = 0; i < t->esc_n; i++) apply_sgr(t, t->esc_args[i]);
    else if (ch == 'J' && t->esc_args[0] == 2) clear_locked(t);
    else if (ch == 'H') t->cursor_col = t->cursor_row = 0;
    t->esc_state = 0;
}

static void putc_locked(struct kestrel_tty *t, char c)
{
    unsigned char ch = (unsigned char)c;
    if (t->esc_state) { escape(t, c); return; }
    switch (ch) {
    case 0x1B: t->esc_state = 1; return;
    case '\n': newline(t); return;
    case '\r': t->cursor_col = 0; return;
    case '\t': do put_glyph(t, ' '); while (t->cursor_col % 8 && t->cursor_col != 0); return;
    case '\b':                                  /* non-destructive, like a VT100 */
        if (t->cursor_col > 0) t->cursor_col--;
        else if (t->cursor_row > 0) { t->cursor_row--; t->cursor_col = t->max_cols - 1; }
        return;
    case 0x07: case 0x00: return;               /* bell, NUL */
    }
    if (ch < 0x20 || ch == 0x7F) return;        /* other control codes: ignore */
    put_glyph(t, ch);                           /* 0x20-0x7E and 0x80-0xFF glyphs */
}

void tty_putc(struct kestrel_tty *t, char c)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    putc_locked(t, c);
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

void tty_write(struct kestrel_tty *t, const char *s, size_t n)
{
    if (!t || !t->active || !s) return;
    while (n) {
        size_t chunk = n < 256 ? n : 256;       /* let other writers of this TTY in between */
        if (!tty_lock(t)) return;               /* busy, and we cannot wait (IRQ context) */
        cursor_hide(t);
        for (size_t i = 0; i < chunk; i++) putc_locked(t, s[i]);
        cursor_refresh(t);
        tty_unlock(t);
        s += chunk; n -= chunk;
    }
}

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

void tty_set_color(struct kestrel_tty *t, uint32_t fg, uint32_t bg) { if (t) { t->fg = fg; t->bg = bg; } }

/* Line-editor backspace: move left (wrapping back over a line break) and
 * overwrite that cell with a blank in the background colour. */
void tty_backspace(struct kestrel_tty *t)
{
    if (!t || !t->active) return;
    if (!tty_lock(t)) return;
    cursor_hide(t);
    if (t->cursor_col > 0) t->cursor_col--;
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
    if (!t->cells) return false;
    t->fg = TTY_FG;
    t->bg = TTY_BG;
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
