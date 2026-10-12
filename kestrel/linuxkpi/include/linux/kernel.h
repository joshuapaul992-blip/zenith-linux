/* linux/kernel.h -- Kestrel's Linux KPI: the grab bag every driver includes */
#ifndef _LKPI_LINUX_KERNEL_H
#define _LKPI_LINUX_KERNEL_H

#include <linux/stdarg.h>
#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include <linux/kstrtox.h>
#include <linux/log2.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/typecheck.h>
#include <linux/types.h>
#include <linux/container_of.h>
#include <linux/printk.h>
#include <linux/sprintf.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/wordpart.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/instruction_pointer.h>
#include <linux/static_call_types.h>
#include <asm/byteorder.h>
#include <asm/div64.h>

#define STACK_MAGIC             0xdeadbeef
#define u64_to_user_ptr(x)      ((void __user *)(uintptr_t)(x))
#define FIELD_SIZEOF(t, f)      (sizeof(((t *)0)->f))
#define SYSTEM_RUNNING          2
#define system_state            SYSTEM_RUNNING

/* Sleeping where it must not: logged, not fatal. */
void __might_sleep(const char *file, int line);
#define might_sleep()           __might_sleep(__FILE__, __LINE__)
#define might_sleep_if(cond)    do { if (cond) might_sleep(); } while (0)
#define might_fault()           do { } while (0)
#define might_resched()         do { } while (0)
#define cant_sleep()            do { } while (0)
#define non_block_start()       do { } while (0)
#define non_block_end()         do { } while (0)

int lkpi_num_online_cpus(void);
#define num_online_cpus()       lkpi_num_online_cpus()
#define num_possible_cpus()     lkpi_num_online_cpus()
#define nr_cpu_ids              lkpi_num_online_cpus()

__printf(1, 2) __noreturn void lkpi_panic(const char *fmt, ...);
#define panic                   lkpi_panic

#define oops_in_progress        0

#endif
