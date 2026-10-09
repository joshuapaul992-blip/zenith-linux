/* include/kernel/kestrel_font.h -- the terminal's 8x16 bitmap font
 *
 * 256 glyphs indexed by byte value (Windows-1252 layout in the default
 * font). Each glyph is 16 bytes, one per pixel row, top to bottom; bit 7 of
 * a row byte is the leftmost pixel.
 *
 * The default definition (drivers/kestrel_font.c) is generated from the
 * glyph sheet in third_party/kestrel_font/ by tools/sheet2font.py. Replace
 * that file with any other array of this shape to change the font. */
#ifndef KESTREL_FONT_ARRAY_H
#define KESTREL_FONT_ARRAY_H

#define KFONT_WIDTH   8
#define KFONT_HEIGHT  16
#define KFONT_GLYPHS  256

extern unsigned char kestrel_font[KFONT_GLYPHS][KFONT_HEIGHT];

#endif
