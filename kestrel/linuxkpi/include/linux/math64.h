/* linux/math64.h -- Kestrel's Linux KPI: 64-bit division (native on x86-64) */
#ifndef _LKPI_LINUX_MATH64_H
#define _LKPI_LINUX_MATH64_H

#include <linux/types.h>
#include <linux/math.h>

static inline u64 div_u64_rem(u64 d, u32 divisor, u32 *rem) { *rem = (u32)(d % divisor); return d / divisor; }
static inline s64 div_s64_rem(s64 d, s32 divisor, s32 *rem) { *rem = (s32)(d % divisor); return d / divisor; }
static inline u64 div64_u64_rem(u64 d, u64 divisor, u64 *rem) { *rem = d % divisor; return d / divisor; }
static inline u64 div_u64(u64 d, u32 divisor) { return d / divisor; }
static inline s64 div_s64(s64 d, s32 divisor) { return d / divisor; }
static inline u64 div64_u64(u64 d, u64 divisor) { return d / divisor; }
static inline s64 div64_s64(s64 d, s64 divisor) { return d / divisor; }
static inline u64 div64_ul(u64 d, unsigned long divisor) { return d / divisor; }
#define div64_long(x, y)        div64_s64((x), (y))
static inline u64 mul_u32_u32(u32 a, u32 b) { return (u64)a * b; }
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned int shift)
{
    return (u64)(((unsigned __int128)a * mul) >> shift);
}
/* a * b / c with a 128-bit intermediate; the quotient must fit 64 bits */
static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
    u64 q, rem;
    __asm__ ("mulq %3; divq %4" : "=a"(q), "=&d"(rem) : "a"(a), "rm"(b), "rm"(c));
    return q;
}
#define DIV64_U64_ROUND_UP(ll, d) ({ u64 _tmp = (d); div64_u64((ll) + _tmp - 1, _tmp); })
#define DIV64_U64_ROUND_CLOSEST(dividend, divisor) ({ u64 _tmp = (divisor); div64_u64((dividend) + _tmp / 2, _tmp); })
#define DIV_U64_ROUND_CLOSEST(dividend, divisor) ({ u32 _tmp = (divisor); div_u64((u64)(dividend) + _tmp / 2, _tmp); })

#endif
