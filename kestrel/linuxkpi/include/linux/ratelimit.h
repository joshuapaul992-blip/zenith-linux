/* linux/ratelimit.h -- Kestrel's Linux KPI: rate limits (not enforced in phase 1) */
#ifndef _LKPI_LINUX_RATELIMIT_H
#define _LKPI_LINUX_RATELIMIT_H
#include <linux/types.h>
#include <linux/spinlock.h>
#define DEFAULT_RATELIMIT_INTERVAL      (5 * HZ)
#define DEFAULT_RATELIMIT_BURST         10
struct ratelimit_state { int interval, burst, printed, missed; unsigned long begin; };
#define RATELIMIT_STATE_INIT(name, i, b) { .interval = (i), .burst = (b) }
#define DEFINE_RATELIMIT_STATE(name, i, b) struct ratelimit_state name = RATELIMIT_STATE_INIT(name, i, b)
static inline void ratelimit_state_init(struct ratelimit_state *rs, int interval, int burst)
{ rs->interval = interval; rs->burst = burst; rs->printed = rs->missed = 0; rs->begin = 0; }
#define __ratelimit(state)      1
#endif
