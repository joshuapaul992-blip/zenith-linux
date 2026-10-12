/* linux/jiffies.h -- Kestrel's Linux KPI: the tick count
 *
 * HZ is 1000, as Kestrel's tick: a jiffy is a millisecond of the
 * monotonic clock. jiffies is read from the clock, so it is exact
 * whether or not a tick has run. */
#ifndef _LKPI_LINUX_JIFFIES_H
#define _LKPI_LINUX_JIFFIES_H

#include <linux/types.h>
#include <linux/limits.h>
#include <linux/typecheck.h>
#include <linux/time64.h>

#define HZ                      CONFIG_HZ
#define USER_HZ                 100
#define MAX_JIFFY_OFFSET        ((LONG_MAX >> 1) - 1)

unsigned long lkpi_jiffies(void);
#define jiffies                 lkpi_jiffies()
#define jiffies_64              ((u64)lkpi_jiffies())
static inline u64 get_jiffies_64(void) { return lkpi_jiffies(); }

#define time_after(a, b)        ((long)((b) - (a)) < 0)
#define time_before(a, b)       time_after(b, a)
#define time_after_eq(a, b)     ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)    time_after_eq(b, a)
#define time_in_range(a, b, c)  (time_after_eq(a, b) && time_before_eq(a, c))
#define time_in_range_open(a, b, c) (time_after_eq(a, b) && time_before(a, c))
#define time_after64(a, b)      ((s64)((b) - (a)) < 0)
#define time_before64(a, b)     time_after64(b, a)
#define time_after_eq64(a, b)   ((s64)((a) - (b)) >= 0)
#define time_before_eq64(a, b)  time_after_eq64(b, a)
#define time_is_before_jiffies(a)       time_after(jiffies, a)
#define time_is_after_jiffies(a)        time_before(jiffies, a)
#define time_is_before_eq_jiffies(a)    time_after_eq(jiffies, a)
#define time_is_after_eq_jiffies(a)     time_before_eq(jiffies, a)

static inline unsigned int jiffies_to_msecs(unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(unsigned long j) { return (unsigned int)(j * 1000); }
static inline u64 jiffies_to_nsecs(unsigned long j) { return (u64)j * NSEC_PER_MSEC; }
static inline u64 jiffies64_to_nsecs(u64 j) { return j * NSEC_PER_MSEC; }
static inline u64 jiffies64_to_msecs(u64 j) { return j; }
/* HZ is 1000: a jiffy is a millisecond, and no unsigned int overflows one */
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
static inline unsigned long usecs_to_jiffies(unsigned int u) { return (u + 999) / 1000; }
static inline unsigned long nsecs_to_jiffies(u64 n) { return (unsigned long)((n + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC); }
static inline u64 nsecs_to_jiffies64(u64 n) { return nsecs_to_jiffies(n); }
static inline unsigned long secs_to_jiffies(unsigned int s) { return (unsigned long)s * HZ; }
static inline clock_t jiffies_to_clock_t(unsigned long x) { return x / (HZ / USER_HZ); }
static inline unsigned long round_jiffies(unsigned long j) { return j; }
static inline unsigned long round_jiffies_relative(unsigned long j) { return j; }
static inline unsigned long round_jiffies_up(unsigned long j) { return j; }
static inline unsigned long round_jiffies_up_relative(unsigned long j) { return j; }
static inline unsigned long __round_jiffies_relative(unsigned long j, int cpu) { return j; }
#define jiffies_delta_to_msecs(d)       jiffies_to_msecs(d)

#endif
