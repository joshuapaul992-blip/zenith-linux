/* linux/math.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_MATH_H
#define _LKPI_LINUX_MATH_H

#include <linux/types.h>
#include <linux/const.h>

#define __round_mask(x, y)      ((__typeof__(x))((y) - 1))
#define round_up(x, y)          ((((x) - 1) | __round_mask(x, y)) + 1)     /* y: a power of 2 */
#define round_down(x, y)        ((x) & ~__round_mask(x, y))
#define DIV_ROUND_UP(n, d)      __KERNEL_DIV_ROUND_UP(n, d)
#define DIV_ROUND_DOWN_ULL(ll, d) ({ unsigned long long _tmp = (ll); _tmp / (d); })
#define DIV_ROUND_UP_ULL(ll, d) DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))
#define roundup(x, y)           ({ typeof(y) __y = (y); (((x) + (__y - 1)) / __y) * __y; })
#define rounddown(x, y)         ({ typeof(x) __x = (x); __x - (__x % (y)); })
#define DIV_ROUND_CLOSEST(x, divisor) ({ \
        typeof(x) __x = (x); typeof(divisor) __d = (divisor); \
        (((typeof(x))-1) > 0 || ((typeof(divisor))-1) > 0 || (((__x) > 0) == ((__d) > 0))) ? \
            (((__x) + ((__d) / 2)) / (__d)) : (((__x) - ((__d) / 2)) / (__d)); })
#define DIV_ROUND_CLOSEST_ULL(x, divisor) ({ \
        typeof(divisor) __d = divisor; unsigned long long _tmp = (x) + (__d) / 2; _tmp / __d; })
#define mult_frac(x, n, d)      ({ typeof(x) q = (x) / (d); typeof(x) r = (x) % (d); (q * (n)) + ((r * (n)) / (d)); })
#define abs(x)                  ({ typeof(x) __a = (x); __a < 0 ? -__a : __a; })
#define abs_diff(a, b)          ({ typeof(a) __a = (a); typeof(b) __b = (b); __a > __b ? __a - __b : __b - __a; })
#define reciprocal_scale(val, ep_ro) ((u32)(((u64)(val) * (ep_ro)) >> 32))

#endif
