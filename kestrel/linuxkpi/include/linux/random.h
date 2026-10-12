/* linux/random.h -- Kestrel's Linux KPI: random numbers (not cryptographic:
 * an xorshift generator seeded from the TSC, linuxkpi/src/lib.c) */
#ifndef _LKPI_LINUX_RANDOM_H
#define _LKPI_LINUX_RANDOM_H
#include <linux/types.h>
u32  get_random_u32(void);
u64  get_random_u64(void);
void get_random_bytes(void *buf, size_t len);
static inline u8 get_random_u8(void) { return (u8)get_random_u32(); }
static inline u16 get_random_u16(void) { return (u16)get_random_u32(); }
static inline unsigned long get_random_long(void) { return (unsigned long)get_random_u64(); }
static inline u32 get_random_u32_below(u32 ceil) { return ceil ? (u32)(((u64)get_random_u32() * ceil) >> 32) : 0; }
static inline u32 get_random_u32_inclusive(u32 floor, u32 ceil) { return floor + get_random_u32_below(ceil - floor + 1); }
#define prandom_u32_max(c)      get_random_u32_below(c)
static inline void add_device_randomness(const void *buf, size_t len) { }
#endif
