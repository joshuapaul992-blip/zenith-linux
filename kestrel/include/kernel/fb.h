/* include/kernel/fb.h -- linear frame buffer graphics primitives */
#ifndef KESTREL_FB_H
#define KESTREL_FB_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/font.h>
#include <kernel/multiboot2.h>

/* Colours are always passed as 0xRRGGBB and converted to the native
 * pixel layout reported by the boot loader. */
#define RGB(r, g, b)    (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define FB_TRANSPARENT  0xFF000000u     /* "no background" for fb_draw_char */

/* Boot-manager palette (classic BIOS bootmgr look) */
#define COL_BLACK       RGB(0x00, 0x00, 0x00)
#define COL_WHITE       RGB(0xFF, 0xFF, 0xFF)
#define COL_LIGHTGRAY   RGB(0xAA, 0xAA, 0xAA)   /* normal text            */
#define COL_SILVER      RGB(0xC0, 0xC0, 0xC0)   /* highlight / title bars  */
#define COL_DARKGRAY    RGB(0x55, 0x55, 0x55)
#define COL_RED         RGB(0xE0, 0x40, 0x40)
#define COL_GREEN       RGB(0x55, 0xD0, 0x55)
#define COL_YELLOW      RGB(0xF0, 0xD0, 0x50)
#define COL_CYAN        RGB(0x55, 0xC8, 0xE0)
#define COL_BLUE        RGB(0x30, 0x60, 0xD0)

struct framebuffer {
    uint64_t phys;              /* physical address from the MB2 tag   */
    uint8_t *front;             /* mapped video memory                 */
    uint8_t *back;              /* off-screen buffer (same layout) or front */
    uint32_t width, height;
    uint32_t pitch;             /* bytes per scan line                 */
    uint32_t bpp;               /* 32 or 24 supported                  */
    uint32_t bytes_pp;
    uint8_t  r_pos, r_size, g_pos, g_size, b_pos, b_size;
    bool     double_buffered;
    bool     ready;
};

extern struct framebuffer g_fb;

bool fb_init(const struct mb2_tag_framebuffer *tag);
void fb_attach_backbuffer(void *mem);  /* pitch*height bytes; enables double buffering */
void fb_detach_backbuffer(void);       /* draw straight to video memory again */
uint64_t fb_size_bytes(void);

/* --- required primitives ------------------------------------------------- */
void put_pixel(int x, int y, uint32_t rgb);
void draw_rect(int x, int y, int w, int h, uint32_t rgb);             /* filled */
void draw_rect_outline(int x, int y, int w, int h, int thickness, uint32_t rgb);
void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, const struct font *f);
int  draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg, const struct font *f);
void draw_hline(int x, int y, int w, uint32_t rgb);

/* --- helpers ------------------------------------------------------------- */
void fb_clear(uint32_t rgb);
void fb_scroll(int y, int h, int dy, uint32_t fill);  /* move band [y,y+h) up by dy */
void fb_flush(void);                                  /* back -> front, whole screen */
void fb_flush_rect(int x, int y, int w, int h);       /* back -> front, region       */
uint32_t fb_get_pixel(int x, int y);
void fb_set_clip(int x, int y, int w, int h);  /* restrict drawing (not flushing) */
void fb_reset_clip(void);
/* Draw straight into video memory, bypassing the back buffer (overlays such
 * as the mouse pointer; fb_flush_rect() restores what was underneath). */
void fb_front_pixel(int x, int y, uint32_t rgb);
/* 0xRRGGBB -> the frame buffer's native pixel value (for direct writers). */
uint32_t fb_native_color(uint32_t rgb);

#endif
