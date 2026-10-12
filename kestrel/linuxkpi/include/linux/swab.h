/* linux/swab.h -- Kestrel's Linux KPI: byte swapping */
#ifndef _LKPI_LINUX_SWAB_H
#define _LKPI_LINUX_SWAB_H
#include <linux/types.h>
#define swab16(x)       ((u16)__builtin_bswap16((u16)(x)))
#define swab32(x)       ((u32)__builtin_bswap32((u32)(x)))
#define swab64(x)       ((u64)__builtin_bswap64((u64)(x)))
#define __swab16(x)     swab16(x)
#define __swab32(x)     swab32(x)
#define __swab64(x)     swab64(x)
#define ___constant_swab16(x) swab16(x)
#define ___constant_swab32(x) swab32(x)
static inline void swab16s(u16 *p) { *p = swab16(*p); }
static inline void swab32s(u32 *p) { *p = swab32(*p); }
static inline void swab64s(u64 *p) { *p = swab64(*p); }
static inline u32 swahw32(u32 x) { return (x << 16) | (x >> 16); }
#endif
