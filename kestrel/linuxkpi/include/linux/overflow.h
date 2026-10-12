/* linux/overflow.h -- Kestrel's Linux KPI: arithmetic that reports overflow */
#ifndef _LKPI_LINUX_OVERFLOW_H
#define _LKPI_LINUX_OVERFLOW_H

#include <linux/compiler.h>
#include <linux/limits.h>

#define check_add_overflow(a, b, d)     __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d)     __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d)     __builtin_mul_overflow(a, b, d)
#define overflows_type(n, T)            ({ typeof(T) __t; __builtin_add_overflow((n), 0, &__t); })

/* sizes that saturate at SIZE_MAX, so a too-big allocation fails */
static inline size_t size_mul(size_t a, size_t b) { size_t r; return __builtin_mul_overflow(a, b, &r) ? SIZE_MAX : r; }
static inline size_t size_add(size_t a, size_t b) { size_t r; return __builtin_add_overflow(a, b, &r) ? SIZE_MAX : r; }
static inline size_t size_sub(size_t a, size_t b)
{ return (a == SIZE_MAX || b == SIZE_MAX || a < b) ? SIZE_MAX : a - b; }
#define array_size(a, b)                size_mul(a, b)
#define array3_size(a, b, c)            size_mul(size_mul(a, b), c)
#define flex_array_size(p, member, count) size_mul(count, sizeof(*(p)->member))
#define struct_size(p, member, count)   size_add(sizeof(*(p)), flex_array_size(p, member, count))
#define struct_size_t(T, member, count) struct_size((T *)NULL, member, count)

#endif
