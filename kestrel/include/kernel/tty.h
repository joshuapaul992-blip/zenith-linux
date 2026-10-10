/* include/kernel/tty.h -- one isolated text terminal per physical monitor
 *
 * Every display head (display.h) gets a struct kestrel_tty with its own
 * frame buffer pointer, resolution, character grid, cursor, colours, escape
 * parser, keyboard queue and line buffer. Nothing in the rendering pipeline
 * refers to a global screen: draw_char() and scroll_screen() take the TTY
 * they act on and compute every address from that TTY's own frame buffer,
 * pitch and size, so drawing or scrolling on one monitor cannot touch
 * another monitor's memory.
 *
 *   rendering   draw_char(tty, ...)           one 8 x 16 glyph cell
 *               scroll_screen(tty)            shift this monitor's text up
 *   layout      tty_putc / tty_write / ...    cursor, wrapping, ANSI colours
 *               tty_write_vt()                a terminal program's output: the
 *                                             Linux console's escape codes,
 *                                             UTF-8 (see "Terminal" below)
 *   cursor      tty_cursor_tick()             timer IRQ: blinks every TTY at
 *                                             its own cell, on its own phase
 *   input       tty_hotkey()                  keyboard ISR: Ctrl+Alt+Fn moves
 *                                             the keyboard focus to TTY n-1
 *               tty_route_key()               keyboard ISR: key -> the focused
 *                                             TTY's queue, wakes its reader
 *               tty_read_key()                a TTY's shell or reader blocks
 *                                             on its own queue only
 *
 * Each task is bound to one TTY (struct tcb.tty, inherited by the threads it
 * creates); tty_current() is the TTY of the calling task, which is where
 * /dev/tty, stdin/stdout of the shell and its utilities go.
 *
 * Locking: every TTY has its own lock, and drawing runs with interrupts
 * enabled. A long scroll or a flood of output on one monitor therefore never
 * holds off the timer: the cursor tick skips only the TTY that is busy
 * (try-lock) and keeps blinking every other monitor, and the scheduler keeps
 * switching between the shells. Callers that run with interrupts disabled
 * (interrupt handlers, the kernel log from them) only try the lock and skip a
 * busy TTY, since its holder cannot run until they return.
 *
 * Terminal: the console's shells run on a pty whose output is drawn with
 * tty_write_vt() (vt.c). That interprets the Linux console subset of
 * ECMA-48 (TERM=linux): cursor movement and addressing, erase in line and
 * display, insert/delete characters and lines, scroll regions, save/restore
 * cursor, SGR (bold, reverse, 8/16/256 colours and 24-bit colour), cursor
 * show/hide, application cursor keys, and the status and cursor-position
 * reports (answered through the `reply` hook). Lines wrap the way xterm and
 * the Linux console do: a character in the last column leaves the cursor
 * there, and the next printable character moves to the next line first.
 * Text is UTF-8; code points the font has (Windows-1252, i.e. Latin-1 plus
 * the curly quotes, dashes, euro sign and so on) use their glyph, and box
 * drawing and arrows get a look-alike. A byte that is not valid UTF-8 shows
 * its own Windows-1252 glyph, so 8-bit text still reads. tty_write() (the
 * kernel log, kush) uses the same parser, except that a newline also returns
 * the carriage, Backspace at the start of a row goes back to the end of the
 * row above, and ESC[2J also homes the cursor. */
#ifndef KESTREL_TTY_H
#define KESTREL_TTY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/display.h>
#include <kernel/keyboard.h>

#define TTY_CELL_W      8               /* kestrel_font glyph size */
#define TTY_CELL_H      16
#define TTY_BLINK_MS    500
#define TTY_INPUT_MAX   1024
#define TTY_KEYQ        64              /* key events buffered per TTY */

/* default colours (0xRRGGBB) */
#define TTY_FG          0xC0C0C0u
#define TTY_BG          0x000000u

struct tty_cell {
    unsigned char ch;
    uint32_t fg, bg;                    /* 0xRRGGBB */
};

struct kestrel_tty {
    /* ---- the monitor --------------------------------------------------- */
    uint32_t *framebuffer_address;      /* this monitor's VRAM aperture             */
    uint32_t *shadow;                   /* RAM copy drawn first, then published to
                                           VRAM (NULL: draw straight into VRAM)     */
    int       native_width, native_height;  /* pixels                               */
    int       pitch;                    /* bytes per scan line (>= width * bpp)     */
    void    (*write_span)(void *ctx, uint32_t offset, const void *src, uint32_t len);
    void     *write_ctx;                /* VRAM without a CPU mapping, see display.h */
    int       bytes_pp;                 /* 4 (x8r8g8b8 and friends) or 3 (24 bpp)   */
    uint8_t   r_pos, g_pos, b_pos;      /* pixel format of this monitor             */
    int       index;                    /* monitor number = system_ttys[] index     */

    /* ---- text grid ----------------------------------------------------- */
    int       max_cols, max_rows;       /* native_width / 8, native_height / 16     */
    int       cursor_col, cursor_row;   /* where the next character goes            */
    struct tty_cell *cells;             /* max_rows x max_cols shadow of the text   */
    uint32_t  fg, bg;                   /* current colours                          */
    int       fg_low;                   /* fg is ANSI colour 0-7 (bold brightens it), else -1 */
    bool      bold, reverse;            /* SGR 1 and 7                              */
    bool      wrap_pending;             /* last column written: wrap before the next */
    int       scroll_top, scroll_bot;   /* scroll region (rows, inclusive)          */
    int       esc_state, esc_val, esc_n, esc_args[16];
    char      esc_priv;                 /* CSI private marker ('?', '>', ...)       */
    uint32_t  utf_cp;                   /* UTF-8 decoder: code point so far          */
    int       utf_left, utf_n;          /* continuation bytes still due / seen       */
    uint8_t   utf_raw[4];               /* the sequence's bytes, for a broken one    */
    struct { int col, row, fg_low; uint32_t fg, bg; bool bold, reverse; } saved;   /* ESC 7 */
    bool      app_cursor;               /* DECCKM: cursor keys send ESC O x          */
    /* Answers to the status and cursor-position reports (ESC[5n, ESC[6n,
     * ESC[c) go to the terminal's input; NULL: not answered. Called with
     * the TTY locked. */
    void    (*reply)(void *ctx, const char *s, size_t n);
    void     *reply_ctx;

    /* ---- cursor -------------------------------------------------------- */
    bool      cursor_visible;           /* blink phase: block is on screen now      */
    bool      cursor_enabled;
    bool      cursor_hidden;            /* ESC[?25l from the program on it          */
    int       cursor_drawn_col, cursor_drawn_row;
    uint64_t  next_blink;               /* uptime_ms() of the next toggle           */

    /* ---- input --------------------------------------------------------- */
    struct key_event keyq[TTY_KEYQ];    /* filled by the keyboard ISR               */
    volatile uint32_t keyq_head, keyq_tail;
    char      input_buffer[TTY_INPUT_MAX];  /* the line being edited on this TTY    */
    int       buffer_index;
    int       line_len, line_pos;       /* /dev/tty: completed line, bytes consumed */
    bool      line_ready;

    bool      active;
    bool      graphics;                 /* a graphics client (X) owns the pixels:
                                           text still updates the cells, but
                                           nothing is drawn until it lets go   */
    volatile int lock;                  /* see "Locking" above                      */
    bool      batch;                    /* inside tty_write: see batch_end() in tty.c */
    int       dirty_x0, dirty_y0, dirty_x1, dirty_y1;   /* rectangles to publish, pixels */
    uint8_t  *row_dirty;                /* rows whose cells changed in this batch   */
    int       pending_up;               /* full-screen scrolls not yet done in pixels */
    bool      repaint_all;              /* other scrolls/clears: redraw every row    */
};

extern struct kestrel_tty system_ttys[MAX_MONITORS];
extern int tty_count;
extern volatile int active_keyboard_tty;    /* index of the TTY with keyboard focus */

/* ---- set-up ---------------------------------------------------------------- */
/* One TTY per registered display head, all cleared; starts the cursor blink
 * and routes keyboard input to the TTYs. Returns the number of TTYs. */
int  tty_init_all(void);
bool tty_ready(void);                   /* at least one TTY is up                   */
struct kestrel_tty *tty_get(int index); /* NULL if out of range                     */
struct kestrel_tty *tty_current(void);  /* the calling task's TTY (NULL if none)    */
void tty_bind_current(int index);       /* bind the calling task to TTY `index`     */

/* ---- rendering (per monitor) ---------------------------------------------- */
/* Draw byte `c` of kestrel_font with its top-left pixel at (x, y) of this
 * TTY's monitor; set bits in fg_color, clear bits in bg_color. Clipped to the
 * monitor's native size. */
void draw_char(struct kestrel_tty *t, char c, int x, int y, uint32_t fg_color, uint32_t bg_color);
/* Move text rows 1..max_rows-1 of this monitor up one row and blank the last
 * row; only this monitor's frame buffer is read or written. */
void scroll_screen(struct kestrel_tty *t);

/* ---- text layout ---------------------------------------------------------- */
void tty_putc(struct kestrel_tty *t, char c);           /* \n \r \t \b ESC[...m ESC[2J ESC[H */
void tty_put_glyph(struct kestrel_tty *t, unsigned char c);
void tty_write(struct kestrel_tty *t, const char *s, size_t n);
/* Output of a program on a terminal (a console pty): pure line feed, no
 * reverse wrap; see "Terminal" above. */
void tty_write_vt(struct kestrel_tty *t, const char *s, size_t n);
/* Back to the power-on state: colours, scroll region, cursor shown, parser
 * idle; the text stays unless `clear`. */
void tty_reset(struct kestrel_tty *t, bool clear);
void tty_puts(struct kestrel_tty *t, const char *s);
void tty_printf(struct kestrel_tty *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void tty_set_color(struct kestrel_tty *t, uint32_t fg, uint32_t bg);
void tty_clear(struct kestrel_tty *t);                  /* blank monitor, cursor 0,0 */
void tty_backspace(struct kestrel_tty *t);              /* step left, blank the cell  */

/* ---- cursor ----------------------------------------------------------------- */
void tty_cursor_tick(void);             /* timer IRQ: every TTY, own position/phase */
void tty_cursor_enable(struct kestrel_tty *t, bool on);

/* ---- keyboard ------------------------------------------------------------- */
/* Keyboard ISR hooks (PS/2 IRQ1 and USB HID). tty_hotkey() consumes
 * Ctrl+Alt+F1..F12 and moves the focus; tty_route_key() queues any other key
 * on the focused TTY. Both return false while TTY input is not active. */
bool tty_hotkey(uint16_t key, uint8_t mods);
bool tty_route_key(const struct key_event *ev);
void tty_set_focus(int index);
bool tty_input_active(void);

bool tty_read_key(struct kestrel_tty *t, struct key_event *ev, bool block);
bool tty_has_key(struct kestrel_tty *t);
void tty_flush_keys(struct kestrel_tty *t);

/* /dev/tty line discipline: edits a line in input_buffer with echo, then
 * hands it out across read() calls. Returns bytes, or 0 at Ctrl+D (EOF). */
long tty_read_line(struct kestrel_tty *t, char *buf, size_t n);

/* ---- kernel console --------------------------------------------------------- */
/* Write to the calling task's TTY (the g_con redirect: /dev/tty, stdout). */
void tty_write_current(const char *s, size_t n);

/* ---- graphics clients (the X server), through /dev/fbN ------------------- */
/* Hand monitor `index` to a graphics client (on) or give it back to the
 * terminal (off: the text is repainted from the cell model). */
int  tty_set_graphics(int index, bool on);
/* Copy a w x h rectangle of 32-bpp pixels (in this monitor's format) from
 * `src` (pitch bytes per row) to (x, y) and show it. Clipped. */
int  tty_blit(int index, int x, int y, int w, int h, const void *src, uint32_t pitch);
#endif
