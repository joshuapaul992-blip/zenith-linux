/* linux/bitmap.h -- Kestrel's Linux KPI: arrays of bits (linuxkpi/src/lib.c) */
#ifndef _LKPI_LINUX_BITMAP_H
#define _LKPI_LINUX_BITMAP_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/string.h>

#define BITMAP_FIRST_WORD_MASK(start)   (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits)    (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{ memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long)); }
static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{ memset(dst, 0xff, BITS_TO_LONGS(nbits) * sizeof(unsigned long)); }
static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{ memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(unsigned long)); }

void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits);
void bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits);
bool bitmap_empty(const unsigned long *src, unsigned int nbits);
bool bitmap_full(const unsigned long *src, unsigned int nbits);
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);
bool bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned int nbits);
void bitmap_complement(unsigned long *dst, const unsigned long *src, unsigned int nbits);
unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size, unsigned long start,
                                         unsigned int nr, unsigned long align_mask);
/* a free, aligned region of 2^order bits: its first bit, or -ENOMEM */
int  bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order);
void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order);
unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags);
unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags);
void bitmap_free(const unsigned long *bitmap);

#endif
