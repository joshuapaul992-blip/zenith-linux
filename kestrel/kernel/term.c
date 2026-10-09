/* kernel/term.c -- interactive 128 x 48 text terminal on the frame buffer
 *
 * Layers (each a separate group of functions below):
 *   1. glyph rendering   draw_char()
 *   2. cell model        a shadow copy of every cell (character + colours)
 *                        so the cursor can restore what it covers and
 *                        scrolling can keep the model in step with pixels
 *   3. cursor            hide/show/blink; blink driven by the PIT tick hook
 *   4. text layout       putc, wrapping, newline, scroll_screen, clear, ANSI
 *
 * Locking: every operation that touches pixels or the cell model runs with
 * interrupts disabled (single CPU). That also serialises it against the
 * cursor blink, which runs in the timer interrupt. Long writes are split
 * into chunks so interrupts are never held off for long. */
#include <kernel/term.h>
#include <kernel/kestrel_font.h>
#include <kernel/fb.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/string.h>

int term_col = 0;
int term_row = 0;

struct cell {
    unsigned char ch;
    uint32_t fg, bg;
};

static bool     active;
static int      cols = TERM_COLS, rows = TERM_ROWS;
static uint32_t cur_fg = TERM_FG, cur_bg = TERM_BG;
static struct cell cells[TERM_ROWS][TERM_COLS];

/* cursor state */
static bool     cursor_enabled = true;
static bool     cursor_drawn;               /* inverse cell currently on screen */
static int      cursor_x, cursor_y;         /* cell where it was drawn          */
static uint64_t next_blink;

/* ANSI escape parser state */
static int esc_state, esc_val, esc_n, esc_args[8];

/* ======================================================================== */
/*  1. glyph rendering                                                         */
/* ======================================================================== */

void draw_char(char c, int x, int y, uint32_t fg_color, uint32_t bg_color)
{
    if (!g_fb.ready) return;
    const unsigned char *glyph = kestrel_font[(unsigned char)c];   /* char may be signed */
    const uint32_t fg = fb_native_color(fg_color);
    const uint32_t bg = fb_native_color(bg_color);
    const uint32_t bpp = g_fb.bytes_pp;

    for (int row = 0; row < KFONT_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)g_fb.height) continue;
        uint8_t *line = g_fb.back + (size_t)py * g_fb.pitch;
        unsigned char bits = glyph[row];
        for (int col = 0; col < KFONT_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= (int)g_fb.width) continue;
            uint32_t v = (bits & (0x80u >> col)) ? fg : bg;     /* bit 7 = leftmost pixel */
            uint8_t *p = line + (size_t)px * bpp;
            if (bpp == 4) {
                *(uint32_t *)p = v;
            } else {                                            /* 24-bpp modes */
                p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
            }
        }
    }
    fb_flush_rect(x, y, KFONT_WIDTH, KFONT_HEIGHT);     /* no-op without a back buffer */
}

/* ======================================================================== */
/*  2. cell model                                                              */
/* ======================================================================== */

static void cell_draw(int cx, int cy, bool inverse)
{
    const struct cell *k = &cells[cy][cx];
    draw_char((char)k->ch, cx * TERM_CELL_W, cy * TERM_CELL_H,
              inverse ? k->bg : k->fg, inverse ? k->fg : k->bg);
}

static void cell_set(int cx, int cy, unsigned char ch)
{
    cells[cy][cx] = (struct cell){ ch, cur_fg, cur_bg };
    cell_draw(cx, cy, false);
}

/* ======================================================================== */
/*  3. cursor                                                                  */
/* ======================================================================== */

/* Callers hold the lock (interrupts off). */
static void cursor_hide(void)
{
    if (!cursor_drawn) return;
    cell_draw(cursor_x, cursor_y, false);       /* restore the glyph underneath */
    cursor_drawn = false;
}

static void cursor_show(void)
{
    if (!active || !cursor_enabled || cursor_drawn) return;
    if (term_col < 0 || term_col >= cols || term_row < 0 || term_row >= rows) return;
    cursor_x = term_col;
    cursor_y = term_row;
    cell_draw(cursor_x, cursor_y, true);        /* solid block: inverse video */
    cursor_drawn = true;
}

/* After any output: cursor visible at the new position, blink restarted, so
 * typing never shows a lagging or invisible cursor. */
static void cursor_refresh(void)
{
    cursor_hide();
    cursor_show();
    next_blink = uptime_ms() + TERM_BLINK_MS;
}

void term_cursor_tick(void)                     /* timer IRQ context */
{
    if (!active || !cursor_enabled) return;
    uint64_t now = uptime_ms();
    if (now < next_blink) return;
    next_blink = now + TERM_BLINK_MS;
    if (cursor_drawn) cursor_hide(); else cursor_show();
}

void term_cursor_enable(bool on)
{
    uint64_t f = irq_save();
    cursor_enabled = on;
    if (on) cursor_refresh(); else cursor_hide();
    irq_restore(f);
}

/* ======================================================================== */
/*  4. text layout                                                             */
/* ======================================================================== */

static void scroll_locked(void)
{
    const size_t pitch = g_fb.pitch;
    const size_t row_bytes = (size_t)cols * TERM_CELL_W * g_fb.bytes_pp;
    const int text_h = rows * TERM_CELL_H;
    uint8_t *fb = g_fb.back;

    /* Shift pixel rows 16..767 up to 0..751. Each scan line is copied on
     * its own: source and destination never overlap, so memcpy is valid. */
    for (int py = 0; py < text_h - TERM_CELL_H; py++)
        memcpy(fb + (size_t)py * pitch, fb + (size_t)(py + TERM_CELL_H) * pitch, row_bytes);
    /* Blank the bottom text row to absolute black. */
    for (int py = text_h - TERM_CELL_H; py < text_h; py++)
        memset(fb + (size_t)py * pitch, 0x00, row_bytes);

    memmove(cells[0], cells[1], sizeof(struct cell) * TERM_COLS * (size_t)(rows - 1));
    for (int x = 0; x < TERM_COLS; x++) cells[rows - 1][x] = (struct cell){ ' ', cur_fg, 0x000000 };

    if (cursor_drawn && cursor_y > 0) cursor_y--;   /* it moved with the pixels */
    else cursor_drawn = false;
    term_row = rows - 1;
    fb_flush_rect(0, 0, cols * TERM_CELL_W, text_h);
}

void scroll_screen(void)
{
    uint64_t f = irq_save();
    cursor_hide();
    scroll_locked();
    cursor_refresh();
    irq_restore(f);
}

static void newline(void)
{
    term_col = 0;
    if (++term_row >= rows) scroll_locked();
}

static void put_glyph(unsigned char ch)
{
    cell_set(term_col, term_row, ch);
    if (++term_col >= cols) newline();          /* wrap to the next row */
}

static void clear_locked(void)
{
    memset(g_fb.back, 0x00, fb_size_bytes());   /* entire 1024x768 surface black */
    fb_flush();
    for (int y = 0; y < TERM_ROWS; y++)
        for (int x = 0; x < TERM_COLS; x++) cells[y][x] = (struct cell){ ' ', cur_fg, 0x000000 };
    term_col = term_row = 0;
    cursor_drawn = false;
}

void term_clear(void)
{
    uint64_t f = irq_save();
    clear_locked();
    cursor_refresh();
    irq_restore(f);
}

/* ANSI SGR subset: 0 reset, 1 bold (bright), 30-37/90-97 fg, 40-47 bg,
 * 39/49 defaults; also ESC[2J (clear) and ESC[H (home). */
static const uint32_t ansi[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xC0C0C0,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static void apply_sgr(int v)
{
    if (v == 0)                   { cur_fg = TERM_FG; cur_bg = TERM_BG; }
    else if (v == 1)              cur_fg = cur_fg == TERM_FG ? 0xFFFFFF : cur_fg;
    else if (v >= 30 && v <= 37)  cur_fg = ansi[v - 30];
    else if (v >= 90 && v <= 97)  cur_fg = ansi[v - 90 + 8];
    else if (v >= 40 && v <= 47)  cur_bg = ansi[v - 40];
    else if (v >= 100 && v <= 107) cur_bg = ansi[v - 100 + 8];
    else if (v == 39)             cur_fg = TERM_FG;
    else if (v == 49)             cur_bg = TERM_BG;
}

static bool escape(char ch)
{
    if (esc_state == 1) {
        if (ch == '[') { esc_state = 2; esc_n = 0; esc_val = -1; return true; }
        esc_state = 0;
        return true;
    }
    if (ch >= '0' && ch <= '9') { esc_val = (esc_val < 0 ? 0 : esc_val) * 10 + (ch - '0'); return true; }
    if (ch == ';') { if (esc_n < 8) esc_args[esc_n++] = esc_val < 0 ? 0 : esc_val; esc_val = -1; return true; }
    if (esc_n < 8) esc_args[esc_n++] = esc_val < 0 ? 0 : esc_val;
    if (ch == 'm') for (int i = 0; i < esc_n; i++) apply_sgr(esc_args[i]);
    else if (ch == 'J' && esc_args[0] == 2) clear_locked();
    else if (ch == 'H') term_col = term_row = 0;
    esc_state = 0;
    return true;
}

static void putc_locked(char c)
{
    unsigned char ch = (unsigned char)c;
    if (esc_state) { escape(c); return; }
    switch (ch) {
    case 0x1B: esc_state = 1; return;
    case '\n': newline(); return;
    case '\r': term_col = 0; return;
    case '\t': do put_glyph(' '); while (term_col % 8 && term_col != 0); return;
    case '\b':                                  /* non-destructive, like a VT100 */
        if (term_col > 0) term_col--;
        else if (term_row > 0) { term_row--; term_col = cols - 1; }
        return;
    case 0x07: case 0x00: return;               /* bell, NUL */
    }
    if (ch < 0x20 || ch == 0x7F) return;        /* other control codes: ignore */
    put_glyph(ch);                              /* 0x20-0x7E and 0x80-0xFF glyphs */
}

void term_putc(char c)
{
    if (!active) return;
    uint64_t f = irq_save();
    cursor_hide();
    putc_locked(c);
    cursor_refresh();
    irq_restore(f);
}

void term_put_glyph(unsigned char c)
{
    if (!active) return;
    uint64_t f = irq_save();
    cursor_hide();
    put_glyph(c);
    cursor_refresh();
    irq_restore(f);
}

void term_write(const char *s, size_t n)
{
    if (!active || !s) return;
    while (n) {
        size_t chunk = n < 256 ? n : 256;       /* bound the interrupts-off window */
        uint64_t f = irq_save();
        cursor_hide();
        for (size_t i = 0; i < chunk; i++) putc_locked(s[i]);
        cursor_refresh();
        irq_restore(f);
        s += chunk; n -= chunk;
    }
}

void term_puts(const char *s) { if (s) term_write(s, strlen(s)); }

void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    term_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

void term_set_color(uint32_t fg, uint32_t bg) { cur_fg = fg; cur_bg = bg; }
void term_reset_color(void) { cur_fg = TERM_FG; cur_bg = TERM_BG; }

/* Line-editor backspace: move left (wrapping back over a line break) and
 * overwrite that cell with a blank in the background colour. */
void term_backspace(void)
{
    if (!active) return;
    uint64_t f = irq_save();
    cursor_hide();
    if (term_col > 0) term_col--;
    else if (term_row > 0) { term_row--; term_col = cols - 1; }
    cell_set(term_col, term_row, ' ');
    cursor_refresh();
    irq_restore(f);
}

bool term_active(void) { return active; }

bool term_init(void)
{
    if (!g_fb.ready || g_fb.width < TERM_CELL_W || g_fb.height < TERM_CELL_H) return false;
    cols = (int)(g_fb.width / TERM_CELL_W);
    rows = (int)(g_fb.height / TERM_CELL_H);
    if (cols > TERM_COLS) cols = TERM_COLS;     /* 1024 x 768 -> exactly 128 x 48 */
    if (rows > TERM_ROWS) rows = TERM_ROWS;
    cur_fg = TERM_FG; cur_bg = TERM_BG;
    esc_state = 0;
    active = true;
    term_clear();
    pit_add_tick_hook(term_cursor_tick);
    return true;
}
