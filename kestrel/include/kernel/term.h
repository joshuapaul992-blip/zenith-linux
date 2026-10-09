/* include/kernel/term.h -- interactive text terminal on the linear frame buffer
 *
 * A 128 x 48 character grid (1024 x 768 pixels, 8 x 16 cells) drawn with
 * kestrel_font[256][16]. Responsibilities are split into four layers:
 *
 *   glyph rendering   draw_char()                       pixels of one cell
 *   text layout       term_putc / term_write / ...      cursor position,
 *                     scroll_screen(), term_clear()     wrapping, scrolling,
 *                                                       ANSI colour codes
 *   cursor            term_cursor_tick()                blink from the timer
 *                                                       interrupt (500 ms)
 *   input editing     term_backspace()                  used by the shell's
 *                                                       line editor (sh.c)
 *
 * All entry points are safe to call from any thread; the cursor tick runs in
 * IRQ context. Drawing targets the frame buffer's drawing surface (the
 * off-screen buffer when double buffering is on, otherwise video memory
 * directly) and every changed region is published to video memory at once.
 */
#ifndef KESTREL_TERM_H
#define KESTREL_TERM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TERM_COLS       128
#define TERM_ROWS       48
#define TERM_CELL_W     8
#define TERM_CELL_H     16
#define TERM_BLINK_MS   500

/* default colours (0xRRGGBB) */
#define TERM_FG         0xC0C0C0u
#define TERM_BG         0x000000u

/* Cursor position in character cells (0-based). Read-only outside term.c. */
extern int term_col;
extern int term_row;

/* ---- glyph rendering ------------------------------------------------------ */
/* Draw byte `c` of kestrel_font with its top-left pixel at (x, y): set bits in
 * fg_color, clear bits in bg_color. Pixels outside the screen are clipped. */
void draw_char(char c, int x, int y, uint32_t fg_color, uint32_t bg_color);

/* ---- text layout ------------------------------------------------------------ */
bool term_init(void);                   /* take over the screen; false if no frame buffer */
bool term_active(void);
void term_putc(char c);                 /* interprets \n \r \t \b and ESC[...m */
void term_put_glyph(unsigned char c);   /* draw any of the 256 glyphs literally */
void term_write(const char *s, size_t n);
void term_puts(const char *s);
void term_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void term_set_color(uint32_t fg, uint32_t bg);
void term_reset_color(void);
void term_clear(void);                  /* whole frame buffer black, cursor to 0,0 */
void scroll_screen(void);               /* move rows 1..47 up one row, blank row 47 */

/* ---- input editing ------------------------------------------------------- */
void term_backspace(void);              /* step left (across a wrap) and blank the cell */

/* ---- cursor ---------------------------------------------------------------- */
void term_cursor_tick(void);            /* timer IRQ: toggles every TERM_BLINK_MS */
void term_cursor_enable(bool on);

#endif
