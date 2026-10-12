/* linux/atomic.h -- Kestrel's Linux KPI: atomic_t, atomic64_t, atomic_long_t
 * (GCC __atomic builtins: locked instructions on x86-64; value-returning
 * operations are full barriers, as in Linux) */
#ifndef _LKPI_LINUX_ATOMIC_H
#define _LKPI_LINUX_ATOMIC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>
#include <asm/cmpxchg.h>

static inline int atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline int atomic_read_acquire(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }
static inline void atomic_set_release(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline void atomic_add(int i, atomic_t *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_add(int i, atomic_t *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_sub(int i, atomic_t *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_sub(int i, atomic_t *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_and(int i, atomic_t *v) { __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_and(int i, atomic_t *v) { return __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_or(int i, atomic_t *v) { __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_or(int i, atomic_t *v) { return __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_xor(int i, atomic_t *v) { __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_xor(int i, atomic_t *v) { return __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_andnot(int i, atomic_t *v) { __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_andnot(int i, atomic_t *v) { return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline int atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_sub_return(int i, atomic_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_inc(atomic_t *v) { atomic_add(1, v); }
static inline void atomic_dec(atomic_t *v) { atomic_sub(1, v); }
static inline int atomic_inc_return(atomic_t *v) { return atomic_add_return(1, v); }
static inline int atomic_dec_return(atomic_t *v) { return atomic_sub_return(1, v); }
static inline int atomic_fetch_inc(atomic_t *v) { return atomic_fetch_add(1, v); }
static inline int atomic_fetch_dec(atomic_t *v) { return atomic_fetch_sub(1, v); }
static inline bool atomic_dec_and_test(atomic_t *v) { return atomic_dec_return(v) == 0; }
static inline bool atomic_inc_and_test(atomic_t *v) { return atomic_inc_return(v) == 0; }
static inline bool atomic_sub_and_test(int i, atomic_t *v) { return atomic_sub_return(i, v) == 0; }
static inline bool atomic_add_negative(int i, atomic_t *v) { return atomic_add_return(i, v) < 0; }
static inline int atomic_xchg(atomic_t *v, int n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); }
static inline int atomic_cmpxchg(atomic_t *v, int old, int n)
{
    __atomic_compare_exchange_n(&v->counter, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline bool atomic_try_cmpxchg(atomic_t *v, int *old, int n)
{
    return __atomic_compare_exchange_n(&v->counter, old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
/* add a unless the value is u; the old value */
static inline int atomic_fetch_add_unless(atomic_t *v, int a, int u)
{
    int c = atomic_read(v);
    do { if (c == u) break; } while (!atomic_try_cmpxchg(v, &c, c + a));
    return c;
}
static inline bool atomic_add_unless(atomic_t *v, int a, int u) { return atomic_fetch_add_unless(v, a, u) != u; }
static inline bool atomic_inc_not_zero(atomic_t *v) { return atomic_add_unless(v, 1, 0); }
static inline int atomic_dec_if_positive(atomic_t *v)
{
    int c = atomic_read(v), d;
    do { d = c - 1; if (d < 0) break; } while (!atomic_try_cmpxchg(v, &c, d));
    return d;
}

static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline s64 atomic64_read_acquire(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }
static inline void atomic64_set_release(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline void atomic64_add(s64 i, atomic64_t *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_add(s64 i, atomic64_t *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_sub(s64 i, atomic64_t *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_sub(s64 i, atomic64_t *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_and(s64 i, atomic64_t *v) { __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_and(s64 i, atomic64_t *v) { return __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_or(s64 i, atomic64_t *v) { __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_or(s64 i, atomic64_t *v) { return __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_xor(s64 i, atomic64_t *v) { __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_xor(s64 i, atomic64_t *v) { return __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_andnot(s64 i, atomic64_t *v) { __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_fetch_andnot(s64 i, atomic64_t *v) { return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_add_return(s64 i, atomic64_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_sub_return(s64 i, atomic64_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_inc(atomic64_t *v) { atomic64_add(1, v); }
static inline void atomic64_dec(atomic64_t *v) { atomic64_sub(1, v); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return atomic64_add_return(1, v); }
static inline s64 atomic64_dec_return(atomic64_t *v) { return atomic64_sub_return(1, v); }
static inline s64 atomic64_fetch_inc(atomic64_t *v) { return atomic64_fetch_add(1, v); }
static inline s64 atomic64_fetch_dec(atomic64_t *v) { return atomic64_fetch_sub(1, v); }
static inline bool atomic64_dec_and_test(atomic64_t *v) { return atomic64_dec_return(v) == 0; }
static inline bool atomic64_inc_and_test(atomic64_t *v) { return atomic64_inc_return(v) == 0; }
static inline bool atomic64_sub_and_test(s64 i, atomic64_t *v) { return atomic64_sub_return(i, v) == 0; }
static inline bool atomic64_add_negative(s64 i, atomic64_t *v) { return atomic64_add_return(i, v) < 0; }
static inline s64 atomic64_xchg(atomic64_t *v, s64 n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_cmpxchg(atomic64_t *v, s64 old, s64 n)
{
    __atomic_compare_exchange_n(&v->counter, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline bool atomic64_try_cmpxchg(atomic64_t *v, s64 *old, s64 n)
{
    return __atomic_compare_exchange_n(&v->counter, old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
/* add a unless the value is u; the old value */
static inline s64 atomic64_fetch_add_unless(atomic64_t *v, s64 a, s64 u)
{
    s64 c = atomic64_read(v);
    do { if (c == u) break; } while (!atomic64_try_cmpxchg(v, &c, c + a));
    return c;
}
static inline bool atomic64_add_unless(atomic64_t *v, s64 a, s64 u) { return atomic64_fetch_add_unless(v, a, u) != u; }
static inline bool atomic64_inc_not_zero(atomic64_t *v) { return atomic64_add_unless(v, 1, 0); }
static inline s64 atomic64_dec_if_positive(atomic64_t *v)
{
    s64 c = atomic64_read(v), d;
    do { d = c - 1; if (d < 0) break; } while (!atomic64_try_cmpxchg(v, &c, d));
    return d;
}

static inline long atomic_long_read(const atomic_long_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic_long_set(atomic_long_t *v, long i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline long atomic_long_read_acquire(const atomic_long_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }
static inline void atomic_long_set_release(atomic_long_t *v, long i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline void atomic_long_add(long i, atomic_long_t *v) { __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_add(long i, atomic_long_t *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_sub(long i, atomic_long_t *v) { __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_sub(long i, atomic_long_t *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_and(long i, atomic_long_t *v) { __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_and(long i, atomic_long_t *v) { return __atomic_fetch_and(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_or(long i, atomic_long_t *v) { __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_or(long i, atomic_long_t *v) { return __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_xor(long i, atomic_long_t *v) { __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_xor(long i, atomic_long_t *v) { return __atomic_fetch_xor(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_andnot(long i, atomic_long_t *v) { __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_fetch_andnot(long i, atomic_long_t *v) { return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_add_return(long i, atomic_long_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic_long_sub_return(long i, atomic_long_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_long_inc(atomic_long_t *v) { atomic_long_add(1, v); }
static inline void atomic_long_dec(atomic_long_t *v) { atomic_long_sub(1, v); }
static inline long atomic_long_inc_return(atomic_long_t *v) { return atomic_long_add_return(1, v); }
static inline long atomic_long_dec_return(atomic_long_t *v) { return atomic_long_sub_return(1, v); }
static inline long atomic_long_fetch_inc(atomic_long_t *v) { return atomic_long_fetch_add(1, v); }
static inline long atomic_long_fetch_dec(atomic_long_t *v) { return atomic_long_fetch_sub(1, v); }
static inline bool atomic_long_dec_and_test(atomic_long_t *v) { return atomic_long_dec_return(v) == 0; }
static inline bool atomic_long_inc_and_test(atomic_long_t *v) { return atomic_long_inc_return(v) == 0; }
static inline bool atomic_long_sub_and_test(long i, atomic_long_t *v) { return atomic_long_sub_return(i, v) == 0; }
static inline bool atomic_long_add_negative(long i, atomic_long_t *v) { return atomic_long_add_return(i, v) < 0; }
static inline long atomic_long_xchg(atomic_long_t *v, long n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); }
static inline long atomic_long_cmpxchg(atomic_long_t *v, long old, long n)
{
    __atomic_compare_exchange_n(&v->counter, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline bool atomic_long_try_cmpxchg(atomic_long_t *v, long *old, long n)
{
    return __atomic_compare_exchange_n(&v->counter, old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
/* add a unless the value is u; the old value */
static inline long atomic_long_fetch_add_unless(atomic_long_t *v, long a, long u)
{
    long c = atomic_long_read(v);
    do { if (c == u) break; } while (!atomic_long_try_cmpxchg(v, &c, c + a));
    return c;
}
static inline bool atomic_long_add_unless(atomic_long_t *v, long a, long u) { return atomic_long_fetch_add_unless(v, a, u) != u; }
static inline bool atomic_long_inc_not_zero(atomic_long_t *v) { return atomic_long_add_unless(v, 1, 0); }
static inline long atomic_long_dec_if_positive(atomic_long_t *v)
{
    long c = atomic_long_read(v), d;
    do { d = c - 1; if (d < 0) break; } while (!atomic_long_try_cmpxchg(v, &c, d));
    return d;
}

#define atomic_cond_read_relaxed(v, c)  smp_cond_load_relaxed(&(v)->counter, (c))
#define atomic_cond_read_acquire(v, c)  smp_cond_load_acquire(&(v)->counter, (c))

#endif
