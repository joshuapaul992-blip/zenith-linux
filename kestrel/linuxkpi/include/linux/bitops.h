/* linux/bitops.h -- Kestrel's Linux KPI: bit operations on unsigned long arrays
 *
 * set_bit and friends are atomic (locked instructions), the __ forms are
 * not. Bit nr of the array is bit (nr % 64) of word nr / 64. */
#ifndef _LKPI_LINUX_BITOPS_H
#define _LKPI_LINUX_BITOPS_H

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/typecheck.h>
#include <asm/barrier.h>

#define __lkpi_word(addr, nr)   ((volatile unsigned long *)(addr) + BIT_WORD(nr))

static inline void set_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_or(__lkpi_word(addr, nr), BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline void clear_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_and(__lkpi_word(addr, nr), ~BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline void change_bit(long nr, volatile unsigned long *addr)
{ __atomic_fetch_xor(__lkpi_word(addr, nr), BIT_MASK(nr), __ATOMIC_RELAXED); }
static inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_or(__lkpi_word(addr, nr), BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_and(__lkpi_word(addr, nr), ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline bool test_and_change_bit(long nr, volatile unsigned long *addr)
{ return (__atomic_fetch_xor(__lkpi_word(addr, nr), BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline void clear_bit_unlock(long nr, volatile unsigned long *addr) { barrier(); clear_bit(nr, addr); }
static inline bool test_and_set_bit_lock(long nr, volatile unsigned long *addr) { return test_and_set_bit(nr, addr); }

static inline void __set_bit(long nr, volatile unsigned long *addr) { *__lkpi_word(addr, nr) |= BIT_MASK(nr); }
static inline void __clear_bit(long nr, volatile unsigned long *addr) { *__lkpi_word(addr, nr) &= ~BIT_MASK(nr); }
static inline void __change_bit(long nr, volatile unsigned long *addr) { *__lkpi_word(addr, nr) ^= BIT_MASK(nr); }
static inline bool __test_and_set_bit(long nr, volatile unsigned long *addr)
{ bool o = *__lkpi_word(addr, nr) & BIT_MASK(nr); __set_bit(nr, addr); return o; }
static inline bool __test_and_clear_bit(long nr, volatile unsigned long *addr)
{ bool o = *__lkpi_word(addr, nr) & BIT_MASK(nr); __clear_bit(nr, addr); return o; }
static inline bool test_bit(long nr, const volatile unsigned long *addr)
{ return (*((const volatile unsigned long *)addr + BIT_WORD(nr)) >> (nr % BITS_PER_LONG)) & 1; }
#define test_bit_acquire(nr, addr)      test_bit(nr, addr)
#define assign_bit(nr, addr, value)     ((value) ? set_bit((nr), (addr)) : clear_bit((nr), (addr)))
#define __assign_bit(nr, addr, value)   ((value) ? __set_bit((nr), (addr)) : __clear_bit((nr), (addr)))

/* first set bit, 1-based; 0 for x == 0 (the libc ffs) */
static inline int ffs(int x) { return __builtin_ffs(x); }
/* last set bit, 1-based; 0 for x == 0 */
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned int fls_long(unsigned long l) { return fls64(l); }
/* 0-based; undefined for 0 */
static inline unsigned long __ffs(unsigned long w) { return __builtin_ctzl(w); }
static inline unsigned long __fls(unsigned long w) { return 63 - __builtin_clzl(w); }
static inline unsigned long ffz(unsigned long w) { return __builtin_ctzl(~w); }
static inline unsigned long __ffs64(u64 w) { return __builtin_ctzll(w); }

/* No libgcc in the kernel, and __builtin_popcount without -mpopcnt calls it */
static inline unsigned long hweight64(u64 w)
{
    w = w - ((w >> 1) & 0x5555555555555555ULL);
    w = (w & 0x3333333333333333ULL) + ((w >> 2) & 0x3333333333333333ULL);
    w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (unsigned long)((w * 0x0101010101010101ULL) >> 56);
}
static inline unsigned int hweight8(unsigned int w)  { return (unsigned int)hweight64(w & 0xff); }
static inline unsigned int hweight16(unsigned int w) { return (unsigned int)hweight64(w & 0xffff); }
static inline unsigned int hweight32(unsigned int w) { return (unsigned int)hweight64(w); }
static inline unsigned long hweight_long(unsigned long w) { return hweight64(w); }

static inline u32 rol32(u32 w, unsigned int s) { return (w << (s & 31)) | (w >> ((-s) & 31)); }
static inline u32 ror32(u32 w, unsigned int s) { return (w >> (s & 31)) | (w << ((-s) & 31)); }
static inline u64 rol64(u64 w, unsigned int s) { return (w << (s & 63)) | (w >> ((-s) & 63)); }
static inline u64 ror64(u64 w, unsigned int s) { return (w >> (s & 63)) | (w << ((-s) & 63)); }
static inline u16 rol16(u16 w, unsigned int s) { return (u16)((w << (s & 15)) | (w >> ((-s) & 15))); }
static inline u16 ror16(u16 w, unsigned int s) { return (u16)((w >> (s & 15)) | (w << ((-s) & 15))); }
static inline s32 sign_extend32(u32 v, int index) { u8 s = 31 - index; return (s32)(v << s) >> s; }
static inline s64 sign_extend64(u64 v, int index) { u8 s = 63 - index; return (s64)(v << s) >> s; }
static inline int get_count_order(unsigned int count) { return count == 0 ? -1 : fls(--count); }
static inline int get_count_order_long(unsigned long l) { return l == 0 ? -1 : (int)fls_long(--l); }

#include <linux/find.h>

#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#define for_each_clear_bit(bit, addr, size) \
    for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))
#define for_each_set_bit_from(bit, addr, size) \
    for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))

#endif
