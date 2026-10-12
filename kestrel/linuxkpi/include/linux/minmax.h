/* linux/minmax.h -- Kestrel's Linux KPI: min/max/clamp without double evaluation */
#ifndef _LKPI_LINUX_MINMAX_H
#define _LKPI_LINUX_MINMAX_H

#include <linux/compiler.h>

#define __lkpi_cmp(op, x, y)    ((x) op (y) ? (x) : (y))
#define __lkpi_cmp_once(op, x, y, ux, uy) \
    ({ typeof(x) ux = (x); typeof(y) uy = (y); __lkpi_cmp(op, ux, uy); })
#define __lkpi_minmax(op, x, y) __lkpi_cmp_once(op, x, y, __UNIQUE_ID(x_), __UNIQUE_ID(y_))

#define min(x, y)               __lkpi_minmax(<, x, y)
#define max(x, y)               __lkpi_minmax(>, x, y)
#define umin(x, y)              min((x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)
#define umax(x, y)              max((x) + 0u + 0ul + 0ull, (y) + 0u + 0ul + 0ull)
#define min3(x, y, z)           min((typeof(x))min(x, y), z)
#define max3(x, y, z)           max((typeof(x))max(x, y), z)
#define min_t(T, x, y)          __lkpi_minmax(<, (T)(x), (T)(y))
#define max_t(T, x, y)          __lkpi_minmax(>, (T)(x), (T)(y))
#define min_not_zero(x, y)      ({ typeof(x) __x = (x); typeof(y) __y = (y); \
                                   __x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })
#define clamp(val, lo, hi)      min((typeof(val))max(val, lo), hi)
#define clamp_t(T, val, lo, hi) min_t(T, max_t(T, val, lo), hi)
#define clamp_val(val, lo, hi)  clamp_t(typeof(val), val, lo, hi)
#define in_range(val, start, len) ((val) >= (start) && (val) - (start) < (len))
#define swap(a, b)              do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#endif
