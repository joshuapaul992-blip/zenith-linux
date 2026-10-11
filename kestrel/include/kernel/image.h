/* include/kernel/image.h -- full-screen artwork compiled into the kernel
 * (palette + run-length pixels, made by tools/img2rle.py) */
#ifndef KESTREL_IMAGE_H
#define KESTREL_IMAGE_H

#include <stdint.h>

struct rle_image {
    uint16_t width, height;
    uint16_t ncolors;
    uint32_t nruns;
    const uint32_t *palette;            /* 0xRRGGBB */
    const uint8_t  *runs;               /* (palette index, length - 1) pairs, row by row */
};

/* Draw img over the whole frame buffer: scaled to fit (aspect kept, nearest
 * pixel), centred, the rest filled with the image's first colour. */
void image_draw_fullscreen(const struct rle_image *img);

extern const struct rle_image halt_screen;   /* "It is now safe to turn off your computer." */

#endif
