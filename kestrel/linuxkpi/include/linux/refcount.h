/* linux/refcount.h -- Kestrel's Linux KPI: reference counts that saturate
 * instead of wrapping (a use-after-free becomes a leak, and a warning) */
#ifndef _LKPI_LINUX_REFCOUNT_H
#define _LKPI_LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/bug.h>

typedef struct refcount_struct { atomic_t refs; } refcount_t;

#define REFCOUNT_INIT(n)        { .refs = ATOMIC_INIT(n) }
#define REFCOUNT_MAX            INT_MAX
#define REFCOUNT_SATURATED      (INT_MIN / 2)

static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned int)atomic_read(&r->refs); }

static inline void __refcount_check(refcount_t *r, int old, int added)
{
    if (old <= 0 || old + added <= 0) {
        atomic_set(&r->refs, REFCOUNT_SATURATED);
        WARN_ONCE(1, "refcount_t: misuse (old %d, %+d)\n", old, added);
    }
}

static inline void refcount_add(int i, refcount_t *r) { __refcount_check(r, atomic_fetch_add(i, &r->refs), i); }
static inline void refcount_inc(refcount_t *r) { refcount_add(1, r); }
static inline bool refcount_add_not_zero(int i, refcount_t *r)
{
    int old = atomic_fetch_add_unless(&r->refs, i, 0);
    if (old) __refcount_check(r, old, i);
    return old != 0;
}
static inline bool refcount_inc_not_zero(refcount_t *r) { return refcount_add_not_zero(1, r); }
static inline bool refcount_sub_and_test(int i, refcount_t *r)
{
    int old = atomic_fetch_sub(i, &r->refs);
    if (old == i) return true;
    if (old < i) { atomic_set(&r->refs, REFCOUNT_SATURATED); WARN_ONCE(1, "refcount_t: underflow\n"); }
    return false;
}
static inline bool refcount_dec_and_test(refcount_t *r) { return refcount_sub_and_test(1, r); }
static inline void refcount_dec(refcount_t *r) { if (refcount_dec_and_test(r)) WARN_ONCE(1, "refcount_t: decrement hit 0\n"); }
static inline bool refcount_dec_not_one(refcount_t *r) { return atomic_add_unless(&r->refs, -1, 1); }

#endif
