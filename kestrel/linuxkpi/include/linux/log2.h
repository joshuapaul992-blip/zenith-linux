/* linux/log2.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_LOG2_H
#define _LKPI_LINUX_LOG2_H

#include <linux/types.h>

static inline __attribute_const__ int __ilog2_u32(u32 n) { return 31 - __builtin_clz(n); }
static inline __attribute_const__ int __ilog2_u64(u64 n) { return 63 - __builtin_clzll(n); }

/* floor(log2(n)); n == 0 gives -1 like Linux */
#define ilog2(n) ((n) ? (sizeof(n) <= 4 ? __ilog2_u32((u32)(n)) : __ilog2_u64((u64)(n))) : -1)

static inline __attribute_const__ bool is_power_of_2(unsigned long n) { return n != 0 && ((n & (n - 1)) == 0); }
static inline __attribute_const__ unsigned long __roundup_pow_of_two(unsigned long n)
{
    return n <= 1 ? 1 : 1UL << (64 - __builtin_clzl(n - 1));
}
static inline __attribute_const__ unsigned long __rounddown_pow_of_two(unsigned long n)
{
    return 1UL << (63 - __builtin_clzl(n));
}
#define roundup_pow_of_two(n)   __roundup_pow_of_two(n)
#define rounddown_pow_of_two(n) __rounddown_pow_of_two(n)
#define order_base_2(n)         ((n) > 1 ? ilog2((n) - 1) + 1 : 0)
#define const_ilog2(n)          ilog2(n)

static inline int get_order(unsigned long size)
{
    if (size <= 4096) return 0;
    return 64 - __builtin_clzl((size - 1) >> 12);
}

#endif
