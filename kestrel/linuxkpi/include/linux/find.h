/* linux/find.h -- Kestrel's Linux KPI: searching bitmaps (linuxkpi/src/lib.c) */
#ifndef _LKPI_LINUX_FIND_H
#define _LKPI_LINUX_FIND_H

#include <linux/types.h>

/* the index of the bit found, or `size` if there is none */
unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
unsigned long find_last_bit(const unsigned long *addr, unsigned long size);
static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{ return find_next_bit(addr, size, 0); }
static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{ return find_next_zero_bit(addr, size, 0); }

#endif
