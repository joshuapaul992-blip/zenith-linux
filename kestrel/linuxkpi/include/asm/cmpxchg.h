/* asm/cmpxchg.h -- Kestrel's Linux KPI: cmpxchg/xchg on any native word */
#ifndef _LKPI_ASM_CMPXCHG_H
#define _LKPI_ASM_CMPXCHG_H

/* the value found at ptr (== old when the exchange happened) */
#define cmpxchg(ptr, old, new) ({ \
        typeof(*(ptr)) __o = (old); \
        __atomic_compare_exchange_n((ptr), &__o, (new), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        __o; })
#define cmpxchg_relaxed(ptr, o, n)      cmpxchg(ptr, o, n)
#define cmpxchg_acquire(ptr, o, n)      cmpxchg(ptr, o, n)
#define cmpxchg_release(ptr, o, n)      cmpxchg(ptr, o, n)
#define cmpxchg64(ptr, o, n)            cmpxchg(ptr, o, n)
#define try_cmpxchg(ptr, pold, new) \
        __atomic_compare_exchange_n((ptr), (pold), (new), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define xchg(ptr, v)                    __atomic_exchange_n((ptr), (v), __ATOMIC_SEQ_CST)
#define xchg_relaxed(ptr, v)            xchg(ptr, v)
#define xchg_acquire(ptr, v)            xchg(ptr, v)
#define xchg_release(ptr, v)            xchg(ptr, v)

#endif
