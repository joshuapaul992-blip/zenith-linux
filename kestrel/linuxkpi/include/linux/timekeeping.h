/* linux/timekeeping.h -- Kestrel's Linux KPI: reading the clocks (linuxkpi/src/time.c) */
#ifndef _LKPI_LINUX_TIMEKEEPING_H
#define _LKPI_LINUX_TIMEKEEPING_H

#include <linux/types.h>
#include <linux/time64.h>

ktime_t ktime_get(void);                /* monotonic */
ktime_t ktime_get_real(void);           /* wall clock */
static inline ktime_t ktime_get_raw(void) { return ktime_get(); }
static inline ktime_t ktime_get_boottime(void) { return ktime_get(); }
static inline ktime_t ktime_get_coarse(void) { return ktime_get(); }
static inline u64 ktime_get_ns(void) { return ktime_get(); }
static inline u64 ktime_get_raw_ns(void) { return ktime_get(); }
static inline u64 ktime_get_mono_fast_ns(void) { return ktime_get(); }
static inline u64 ktime_get_boottime_ns(void) { return ktime_get(); }
static inline u64 ktime_get_real_ns(void) { return ktime_get_real(); }
static inline time64_t ktime_get_seconds(void) { return ktime_get() / NSEC_PER_SEC; }
static inline time64_t ktime_get_real_seconds(void) { return ktime_get_real() / NSEC_PER_SEC; }
static inline time64_t ktime_get_boottime_seconds(void) { return ktime_get_seconds(); }
static inline void ktime_get_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(ktime_get()); }
static inline void ktime_get_real_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64(ktime_get_real()); }
static inline void ktime_get_raw_ts64(struct timespec64 *ts) { ktime_get_ts64(ts); }
static inline u64 local_clock(void) { return ktime_get(); }
static inline u64 sched_clock(void) { return ktime_get(); }

#endif
