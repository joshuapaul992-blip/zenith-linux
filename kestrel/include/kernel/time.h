/* include/kernel/time.h -- calibrated monotonic clock, usable before interrupts
 *
 * Source, in order of preference:
 *   1. an invariant TSC (CPUID 0x80000007), calibrated against the HPET:
 *      read in a few cycles, also by the vDSO in user mode (vdso.h)
 *   2. HPET main counter (found through the ACPI HPET table; the period is
 *      given by the hardware, no calibration needed)
 *   3. TSC, calibrated against PIT channel 2 (polled, no IRQ involved)
 * kestrel.clocksource=hpet or =tsc overrides the choice. If the CPUs' TSCs
 * turn out not to be in step (smp.c checks each CPU as it starts), the
 * clock goes back to the HPET.
 * Either works with interrupts disabled, which is what boot-time device
 * settling needs: the 1 kHz tick only runs after sti. uptime_ms() reads
 * this clock too, and counts ticks only when there is none. */
#ifndef KESTREL_TIME_H
#define KESTREL_TIME_H

#include <stdint.h>
#include <stdbool.h>

void        time_init(uintptr_t mbi);   /* after pmm/heap (maps the HPET) */
uint64_t    time_ns(void);              /* monotonic since time_init()     */
uint64_t    time_us(void);
uint64_t    time_ms(void);
/* An ACPI table by signature ("APIC", "HPET", ...), checksum verified; NULL if absent. */
const void *acpi_find_table(const char sig[4]);
const void *acpi_map_table(uint64_t phys);       /* a table not in the root (the DSDT), checked */
void        udelay(uint32_t us);        /* busy-wait, interrupts may be off */
void        mdelay(uint32_t ms);
const char *time_source(void);          /* "HPET 100.000 MHz", "TSC 2.394 GHz (PIT-calibrated)" */
uint64_t    tsc_hz(void);               /* 0 if not calibrated             */
/* The TSC as the clock: its start and rate; false when the clock is not the TSC. */
bool        time_tsc_clock(uint64_t *start, uint64_t *hz);
int64_t     time_boot_epoch(void);      /* realtime = this + time_ns() / 1e9 */
void        time_tsc_unstable(void);    /* smp.c: the TSCs disagree; use the HPET */
int64_t     time_realtime_sec(void);    /* Unix time from the CMOS RTC (UTC) */
void        time_realtime(int64_t *sec, int64_t *nsec);

#endif
