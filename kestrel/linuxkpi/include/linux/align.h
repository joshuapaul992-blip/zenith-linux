/* linux/align.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_ALIGN_H
#define _LKPI_LINUX_ALIGN_H
#include <linux/const.h>
#define ALIGN(x, a)             __ALIGN_KERNEL((x), (a))
#define ALIGN_DOWN(x, a)        __ALIGN_KERNEL((x) - ((a) - 1), (a))
#define __ALIGN_MASK(x, mask)   __ALIGN_KERNEL_MASK((x), (mask))
#define PTR_ALIGN(p, a)         ((typeof(p))ALIGN((unsigned long)(p), (a)))
#define PTR_ALIGN_DOWN(p, a)    ((typeof(p))ALIGN_DOWN((unsigned long)(p), (a)))
#define IS_ALIGNED(x, a)        (((x) & ((typeof(x))(a) - 1)) == 0)
#endif
