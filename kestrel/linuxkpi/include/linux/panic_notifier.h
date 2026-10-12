/* linux/panic_notifier.h -- Kestrel's Linux KPI: called from Kestrel's panic(), interrupts off */
#ifndef _LKPI_LINUX_PANIC_NOTIFIER_H
#define _LKPI_LINUX_PANIC_NOTIFIER_H
#include <linux/notifier.h>
extern struct atomic_notifier_head panic_notifier_list;
extern bool crash_kexec_post_notifiers;
#endif
