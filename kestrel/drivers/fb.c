/* drivers/fb.c -- drawing primitives on the VBE/GOP linear frame buffer.
 *
 * All drawing goes to `g_fb.back`. Before the heap exists this simply
 * aliases video memory; once kernel_main() attaches an off-screen buffer,
 * drawing becomes flicker-free and callers publish changes with
 * fb_flush()/fb_flush_rect(). Both 32-bpp and 24-bpp RGB modes work. */
#include <kernel/fb.h>
#include <kernel/string.h>

struct framebuffer g_fb;

/* Optional clip rectangle: all drawing is restricted to it when set. */
static int clip_x0, clip_y0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;

void fb_set_clip(int x, int y, int w, int h) { clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h; }
void fb_reset_clip(void) { clip_x0 = clip_y0 = 0; clip_x1 = clip_y1 = 1 << 30; }

static inline bool in_clip(int x, int y)
{
    return x >= clip_x0 && y >= clip_y0 && x < clip_x1 && y < clip_y1;
}

static inline uint32_t to_native(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - g_fb.r_size)) << g_fb.r_pos) |
           ((g >> (8 - g_fb.g_size)) << g_fb.g_pos) |
           ((b >> (8 - g_fb.b_size)) << g_fb.b_pos);
}

static inline uint32_t from_native(uint32_t px)
{
    uint32_t r = (px >> g_fb.r_pos) & ((1u << g_fb.r_size) - 1);
    uint32_t g = (px >> g_fb.g_pos) & ((1u << g_fb.g_size) - 1);
    uint32_t b = (px >> g_fb.b_pos) & ((1u << g_fb.b_size) - 1);
    return RGB(r << (8 - g_fb.r_size), g << (8 - g_fb.g_size), b << (8 - g_fb.b_size));
}

bool fb_init(const struct mb2_tag_framebuffer *t)
{
    if (!t || t->fb_type != MB2_FB_TYPE_RGB) return false;
    if (t->bpp != 32 && t->bpp != 24) return false;

    g_fb.phys     = t->addr;
    g_fb.front    = (uint8_t *)(uintptr_t)t->addr;   /* identity mapped (see vmm.c) */
    g_fb.back     = g_fb.front;
    g_fb.width    = t->width;
    g_fb.height   = t->height;
    g_fb.pitch    = t->pitch;
    g_fb.bpp      = t->bpp;
    g_fb.bytes_pp = t->bpp / 8;
    g_fb.r_pos = t->red_pos;   g_fb.r_size = t->red_size;
    g_fb.g_pos = t->green_pos; g_fb.g_size = t->green_size;
    g_fb.b_pos = t->blue_pos;  g_fb.b_size = t->blue_size;
    g_fb.double_buffered = false;
    g_fb.ready = true;
    return true;
}

uint64_t fb_size_bytes(void) { return (uint64_t)g_fb.pitch * g_fb.height; }

void fb_attach_backbuffer(void *mem)
{
    if (!mem) return;
    g_fb.back = mem;
    memcpy(g_fb.back, g_fb.front, fb_size_bytes());
    g_fb.double_buffered = true;
}

void fb_detach_backbuffer(void)
{
    if (g_fb.double_buffered) memcpy(g_fb.front, g_fb.back, fb_size_bytes());
    g_fb.back = g_fb.front;
    g_fb.double_buffered = false;
}

static inline uint8_t *pix_addr(int x, int y)
{
    return g_fb.back + (uint32_t)y * g_fb.pitch + (uint32_t)x * g_fb.bytes_pp;
}

static inline void store(uint8_t *p, uint32_t native)
{
    if (g_fb.bytes_pp == 4) *(uint32_t *)p = native;
    else { p[0] = native & 0xFF; p[1] = (native >> 8) & 0xFF; p[2] = (native >> 16) & 0xFF; }
}

void put_pixel(int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height || !in_clip(x, y)) return;
    store(pix_addr(x, y), to_native(rgb));
}

uint32_t fb_get_pixel(int x, int y)
{
    if ((unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height) return 0;
    uint8_t *p = pix_addr(x, y);
    uint32_t v = g_fb.bytes_pp == 4 ? *(uint32_t *)p : (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    return from_native(v);
}

/* Clip a rectangle to the screen; returns false if nothing is visible. */
static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (int)g_fb.width)  *w = (int)g_fb.width - *x;
    if (*y + *h > (int)g_fb.height) *h = (int)g_fb.height - *y;
    return *w > 0 && *h > 0;
}

/* Intersect with the user clip rectangle as well as the screen. */
static bool clip_draw(int *x, int *y, int *w, int *h)
{
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < clip_x0) *x = clip_x0;
    if (*y < clip_y0) *y = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    *w = x1 - *x; *h = y1 - *y;
    return clip(x, y, w, h);
}

void draw_rect(int x, int y, int w, int h, uint32_t rgb)
{
    if (!clip_draw(&x, &y, &w, &h)) return;
    uint32_t n = to_native(rgb);
    for (int row = 0; row < h; row++) {
        uint8_t *p = pix_addr(x, y + row);
        if (g_fb.bytes_pp == 4) {
            uint32_t *q = (uint32_t *)p;
            for (int i = 0; i < w; i++) q[i] = n;
        } else {
            for (int i = 0; i < w; i++, p += 3) store(p, n);
        }
    }
}

void draw_hline(int x, int y, int w, uint32_t rgb) { draw_rect(x, y, w, 1, rgb); }

void draw_rect_outline(int x, int y, int w, int h, int t, uint32_t rgb)
{
    draw_rect(x, y, w, t, rgb);
    draw_rect(x, y + h - t, w, t, rgb);
    draw_rect(x, y, t, h, rgb);
    draw_rect(x + w - t, y, t, h, rgb);
}

void fb_draw_char(int x, int y, char ch, uint32_t fg, uint32_t bg, const struct font *f)
{
    unsigned char c = (unsigned char)ch;
    if (c < f->first || c > f->last) c = '?';
    const uint8_t *glyph = f->data + (size_t)(c - f->first) * f->height * f->bytes_per_row;
    uint32_t nfg = to_native(fg);
    uint32_t nbg = to_native(bg & 0xFFFFFF);
    bool opaque = bg != FB_TRANSPARENT;

    for (int row = 0; row < f->height; row++) {
        int py = y + row;
        if ((unsigned)py >= g_fb.height) continue;
        uint32_t bits = 0;
        for (int b = 0; b < f->bytes_per_row; b++) bits = (bits << 8) | glyph[row * f->bytes_per_row + b];
        int topbit = f->bytes_per_row * 8 - 1;
        for (int col = 0; col < f->width; col++) {
            int px = x + col;
            if ((unsigned)px >= g_fb.width || !in_clip(px, py)) continue;
            if (bits & (1u << (topbit - col)))
                store(pix_addr(px, py), nfg);
            else if (opaque)
                store(pix_addr(px, py), nbg);
        }
    }
}

int draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg, const struct font *f)
{
    int x0 = x;
    for (; *s; s++) {
        if (*s == '\n') { x = x0; y += f->height; continue; }
        fb_draw_char(x, y, *s, fg, bg, f);
        x += f->width;
    }
    return x;
}

void fb_clear(uint32_t rgb) { draw_rect(0, 0, (int)g_fb.width, (int)g_fb.height, rgb); }

void fb_scroll(int y, int h, int dy, uint32_t fill)
{
    if (dy <= 0 || h <= 0) return;
    if (dy >= h) { draw_rect(0, y, (int)g_fb.width, h, fill); return; }
    memmove(g_fb.back + (uint32_t)y * g_fb.pitch,
            g_fb.back + (uint32_t)(y + dy) * g_fb.pitch,
            (size_t)(h - dy) * g_fb.pitch);
    draw_rect(0, y + h - dy, (int)g_fb.width, dy, fill);
}

void fb_flush_rect(int x, int y, int w, int h)
{
    if (!g_fb.double_buffered || !clip(&x, &y, &w, &h)) return;
    size_t off = (size_t)y * g_fb.pitch + (size_t)x * g_fb.bytes_pp;
    size_t span = (size_t)w * g_fb.bytes_pp;
    for (int row = 0; row < h; row++, off += g_fb.pitch)
        memcpy(g_fb.front + off, g_fb.back + off, span);
}

uint32_t fb_native_color(uint32_t rgb) { return to_native(rgb); }

void fb_front_pixel(int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= g_fb.width || (unsigned)y >= g_fb.height) return;
    uint8_t *p = g_fb.front + (uint32_t)y * g_fb.pitch + (uint32_t)x * g_fb.bytes_pp;
    uint32_t n = to_native(rgb);
    if (g_fb.bytes_pp == 4) *(volatile uint32_t *)p = n;
    else { p[0] = n & 0xFF; p[1] = (n >> 8) & 0xFF; p[2] = (n >> 16) & 0xFF; }
}

void fb_flush(void) { fb_flush_rect(0, 0, (int)g_fb.width, (int)g_fb.height); }
