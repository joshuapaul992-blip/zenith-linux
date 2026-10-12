/* linux/delay.h -- Kestrel's Linux KPI: busy waits and sleeps (linuxkpi/src/time.c) */
#ifndef _LKPI_LINUX_DELAY_H
#define _LKPI_LINUX_DELAY_H

#include <linux/types.h>
#include <linux/math.h>

void lkpi_udelay(unsigned int us);
#define udelay(n)               lkpi_udelay(n)
#define ndelay(n)               lkpi_udelay(DIV_ROUND_UP((n), 1000))
#define mdelay(n)               ({ unsigned long __ms = (n); while (__ms--) lkpi_udelay(1000); })

void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state);
static inline void usleep_range(unsigned long min, unsigned long max) { usleep_range_state(min, max, 0x2); }
static inline void usleep_idle_range(unsigned long min, unsigned long max) { usleep_range_state(min, max, 0x402); }
static inline void ssleep(unsigned int seconds) { msleep(seconds * 1000); }
/* "flexible sleep": spin for short waits, sleep for long ones */
static inline void fsleep(unsigned long usecs)
{
    if (usecs <= 10) udelay(usecs);
    else if (usecs <= 20000) usleep_range(usecs, 2 * usecs);
    else msleep(DIV_ROUND_UP(usecs, 1000));
}

#endif
