/* linux/bug.h -- Kestrel's Linux KPI: BUG (a Kestrel panic) and WARN (a logged message) */
#ifndef _LKPI_LINUX_BUG_H
#define _LKPI_LINUX_BUG_H

#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/printk.h>

__noreturn void lkpi_bug(const char *file, int line);
void lkpi_warn(const char *file, int line);

#define BUG()                   lkpi_bug(__FILE__, __LINE__)
#define BUG_ON(cond)            do { if (unlikely(cond)) BUG(); } while (0)
#define WARN(cond, fmt...)      ({ int __c = !!(cond); \
                                   if (unlikely(__c)) { printk(KERN_WARNING fmt); lkpi_warn(__FILE__, __LINE__); } \
                                   unlikely(__c); })
#define WARN_ON(cond)           ({ int __c = !!(cond); if (unlikely(__c)) lkpi_warn(__FILE__, __LINE__); unlikely(__c); })
#define WARN_ONCE(cond, fmt...) ({ static bool __warned; int __c = !!(cond); \
                                   if (unlikely(__c) && !__warned) { __warned = true; WARN(1, fmt); } \
                                   unlikely(__c); })
#define WARN_ON_ONCE(cond)      ({ static bool __warned; int __c = !!(cond); \
                                   if (unlikely(__c) && !__warned) { __warned = true; lkpi_warn(__FILE__, __LINE__); } \
                                   unlikely(__c); })
#define WARN_TAINT(cond, taint, fmt...)         WARN(cond, fmt)
#define WARN_TAINT_ONCE(cond, taint, fmt...)    WARN_ONCE(cond, fmt)
#define WARN_ON_SMP(cond)       WARN_ON(cond)
#define TAINT_FIRMWARE_WORKAROUND 11
#define TAINT_WARN              9
#define LOCKDEP_STILL_OK        0
static inline void add_taint(unsigned int flag, int lockdep_ok) { }

#endif
