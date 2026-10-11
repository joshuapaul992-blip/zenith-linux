/* include/kernel/vdso.h -- the vDSO: clock reads without a system call
 *
 * Every process gets two read-only pages from the kernel, mapped just
 * above its stack and named in the auxiliary vector (AT_SYSINFO_EHDR):
 *
 *   [vvar]   struct vdso_data, written only by the kernel
 *   [vdso]   a small ELF shared object (vdso/vdso.c) exporting, with
 *            version LINUX_2.6 as on Linux, __vdso_clock_gettime,
 *            __vdso_gettimeofday, __vdso_time, __vdso_clock_getres and
 *            __vdso_getcpu
 *
 * glibc and musl find the functions themselves and call them instead of
 * the system calls. With the TSC as the kernel's clock (an invariant TSC,
 * kernel/time.c) they compute the time from it exactly as time_ns() does,
 * so the two never disagree; otherwise, or for the CPU-time clocks, they
 * make the system call. getcpu() reads IA32_TSC_AUX, which holds the CPU
 * number, with RDPID or RDTSCP where the processor has them.
 *
 * This header is shared with vdso/vdso.c, which is built freestanding. */
#ifndef KESTREL_VDSO_H
#define KESTREL_VDSO_H

#include <stdint.h>

#define VDSO_CLOCK_SYSCALL  0       /* ask the kernel                      */
#define VDSO_CLOCK_TSC      1       /* time_ns() = TSC since tsc_start / hz */

#define VDSO_CPU_SYSCALL    0
#define VDSO_CPU_RDPID      1
#define VDSO_CPU_RDTSCP     2

struct vdso_data {
    volatile uint32_t seq;          /* odd while the kernel updates it      */
    uint32_t clock_mode;            /* VDSO_CLOCK_*                         */
    uint64_t tsc_start, tsc_hz;
    int64_t  boot_epoch;            /* CLOCK_REALTIME = boot_epoch s + monotonic */
    uint32_t cpu_mode;              /* VDSO_CPU_*                           */
};

/* Where the pages go in every address space (uvm.h: above the stack) */
#define VDSO_VVAR_ADDR  0x00007fffff800000ull
#define VDSO_ADDR       (VDSO_VVAR_ADDR + 4096)

#ifndef VDSO_BUILD
struct mm;
void     vdso_init(void);                   /* after vm_init and time_init */
void     vdso_update(void);                 /* the clock changed (time.c)  */
void     vdso_cpu_init(void);               /* each CPU: TSC_AUX = its number */
uint64_t vdso_map(struct mm *mm);           /* exec: map both; the vDSO's address, 0 if none */
#endif

#endif
