/* linux/ktime.h -- Kestrel's Linux KPI: ktime_t, nanoseconds of the monotonic clock */
#ifndef _LKPI_LINUX_KTIME_H
#define _LKPI_LINUX_KTIME_H

#include <linux/types.h>
#include <linux/time64.h>
#include <linux/jiffies.h>
#include <linux/math64.h>

static inline ktime_t ktime_set(s64 secs, unsigned long nsecs) { return secs * NSEC_PER_SEC + (s64)nsecs; }
#define ktime_add(a, b)         ((a) + (b))
#define ktime_sub(a, b)         ((a) - (b))
#define ktime_add_ns(kt, ns)    ((kt) + (ns))
#define ktime_sub_ns(kt, ns)    ((kt) - (ns))
#define ktime_add_us(kt, us)    ((kt) + (s64)(us) * NSEC_PER_USEC)
#define ktime_add_ms(kt, ms)    ((kt) + (s64)(ms) * NSEC_PER_MSEC)
#define ktime_sub_us(kt, us)    ((kt) - (s64)(us) * NSEC_PER_USEC)
static inline s64 ktime_to_ns(ktime_t kt) { return kt; }
static inline s64 ktime_to_us(ktime_t kt) { return kt / NSEC_PER_USEC; }
static inline s64 ktime_to_ms(ktime_t kt) { return kt / NSEC_PER_MSEC; }
static inline ktime_t ns_to_ktime(u64 ns) { return ns; }
static inline ktime_t us_to_ktime(u64 us) { return us * NSEC_PER_USEC; }
static inline ktime_t ms_to_ktime(u64 ms) { return ms * NSEC_PER_MSEC; }
static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier) { return ktime_to_us(later - earlier); }
static inline s64 ktime_ms_delta(ktime_t later, ktime_t earlier) { return ktime_to_ms(later - earlier); }
static inline int ktime_compare(ktime_t a, ktime_t b) { return a < b ? -1 : a > b ? 1 : 0; }
static inline bool ktime_after(ktime_t a, ktime_t b) { return a > b; }
static inline bool ktime_before(ktime_t a, ktime_t b) { return a < b; }
static inline struct timespec64 ktime_to_timespec64(ktime_t kt) { return ns_to_timespec64(kt); }
static inline ktime_t timespec64_to_ktime(struct timespec64 ts) { return ktime_set(ts.tv_sec, ts.tv_nsec); }

#include <linux/timekeeping.h>

#endif
