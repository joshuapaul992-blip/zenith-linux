/* linux/time64.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_TIME64_H
#define _LKPI_LINUX_TIME64_H
#include <linux/types.h>
#define MSEC_PER_SEC    1000L
#define USEC_PER_MSEC   1000L
#define NSEC_PER_USEC   1000L
#define NSEC_PER_MSEC   1000000L
#define USEC_PER_SEC    1000000L
#define NSEC_PER_SEC    1000000000L
#define PSEC_PER_NSEC   1000L
#define FSEC_PER_SEC    1000000000000000LL
#define KTIME_MAX       ((s64)~((u64)1 << 63))
#define KTIME_SEC_MAX   (KTIME_MAX / NSEC_PER_SEC)
#define TIME64_MAX      ((s64)~((u64)1 << 63))
struct timespec64 { time64_t tv_sec; long tv_nsec; };
static inline struct timespec64 ns_to_timespec64(s64 nsec)
{ struct timespec64 ts = { nsec / NSEC_PER_SEC, nsec % NSEC_PER_SEC }; return ts; }
static inline s64 timespec64_to_ns(const struct timespec64 *ts) { return ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec; }
#endif
