/* linux/bits.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_BITS_H
#define _LKPI_LINUX_BITS_H

#include <linux/const.h>

#define BITS_PER_LONG           64
#define BITS_PER_LONG_LONG      64
#define BITS_PER_BYTE           8
#define BITS_PER_TYPE(t)        (sizeof(t) * BITS_PER_BYTE)
#define BIT(nr)                 (UL(1) << (nr))
#define BIT_ULL(nr)             (ULL(1) << (nr))
#define BIT_MASK(nr)            (UL(1) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)        (ULL(1) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)        ((nr) / BITS_PER_LONG_LONG)
#define BITS_TO_LONGS(nr)       __KERNEL_DIV_ROUND_UP(nr, BITS_PER_LONG)
#define BITS_TO_U32(nr)         __KERNEL_DIV_ROUND_UP(nr, 32)
#define BITS_TO_BYTES(nr)       __KERNEL_DIV_ROUND_UP(nr, 8)
/* bits h..l set, h >= l */
#define GENMASK(h, l)           (((~UL(0)) - (UL(1) << (l)) + 1) & (~UL(0) >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)       (((~ULL(0)) - (ULL(1) << (l)) + 1) & (~ULL(0) >> (BITS_PER_LONG_LONG - 1 - (h))))

#endif
