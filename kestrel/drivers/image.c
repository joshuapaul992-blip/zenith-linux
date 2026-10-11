/* drivers/image.c -- full-screen artwork from the kernel image (image.h)
 *
 * Runs are drawn as rectangles straight into the frame buffer: no buffer
 * is needed, so this also works when the system is going down. Scaling is
 * nearest-pixel (each source pixel becomes the block of screen pixels it
 * covers), which keeps flat artwork crisp at any size. */
#include <kernel/image.h>
#include <kernel/fb.h>

void image_draw_fullscreen(const struct rle_image *img)
{
    if (!g_fb.ready || !img->width || !img->height) return;
    int W = (int)g_fb.width, H = (int)g_fb.height, iw = img->width, ih = img->height;
    int dw, dh;                                         /* fit, aspect ratio kept */
    if ((long)W * ih >= (long)H * iw) { dh = H; dw = (int)((long)H * iw / ih); }
    else { dw = W; dh = (int)((long)W * ih / iw); }
    int ox = (W - dw) / 2, oy = (H - dh) / 2;
    draw_rect(0, 0, W, H, img->palette[0]);             /* the background colour */
    int sx = 0, sy = 0;
    for (uint32_t r = 0; r < img->nruns; r++) {
        uint32_t color = img->palette[img->runs[2 * r]];
        int n = img->runs[2 * r + 1] + 1;
        while (n > 0) {                                 /* a run may continue on the next row */
            int k = n < iw - sx ? n : iw - sx;
            if (color != img->palette[0]) {
                int x0 = ox + (int)((long)sx * dw / iw), x1 = ox + (int)((long)(sx + k) * dw / iw);
                int y0 = oy + (int)((long)sy * dh / ih), y1 = oy + (int)((long)(sy + 1) * dh / ih);
                if (x1 > x0 && y1 > y0) draw_rect(x0, y0, x1 - x0, y1 - y0, color);
            }
            n -= k;
            sx += k;
            if (sx == iw) { sx = 0; sy++; }
        }
    }
    fb_flush();
}
