/* include/kernel/time.h -- calibrated monotonic clock, usable before interrupts
 *
 * Source, in order of preference:
 *   1. HPET main counter (found through the ACPI HPET table; the period is
 *      given by the hardware, no calibration needed)
 *   2. TSC, calibrated against PIT channel 2 (polled, no IRQ involved)
 * Either works with interrupts disabled, which is what boot-time device
 * settling needs: the 1 kHz PIT tick (uptime_ms) only runs after sti. */
#ifndef KESTREL_TIME_H
#define KESTREL_TIME_H

#include <stdint.h>
#include <stdbool.h>

void        time_init(uintptr_t mbi);   /* after pmm/heap (maps the HPET) */
uint64_t    time_ns(void);              /* monotonic since time_init()     */
uint64_t    time_us(void);
uint64_t    time_ms(void);
void        udelay(uint32_t us);        /* busy-wait, interrupts may be off */
void        mdelay(uint32_t ms);
const char *time_source(void);          /* "HPET 100.000 MHz", "TSC 2.394 GHz (PIT-calibrated)" */
uint64_t    tsc_hz(void);               /* 0 if not calibrated             */
int64_t     time_realtime_sec(void);    /* Unix time from the CMOS RTC (UTC) */
void        time_realtime(int64_t *sec, int64_t *nsec);

#endif
