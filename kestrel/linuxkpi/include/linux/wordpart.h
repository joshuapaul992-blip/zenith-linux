/* linux/wordpart.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_WORDPART_H
#define _LKPI_LINUX_WORDPART_H
#define upper_32_bits(n)        ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n)        ((u32)((n) & 0xffffffff))
#define upper_16_bits(n)        ((u16)((n) >> 16))
#define lower_16_bits(n)        ((u16)((n) & 0xffff))
#define REPEAT_BYTE(x)          ((~0ul / 0xff) * (x))
#endif
