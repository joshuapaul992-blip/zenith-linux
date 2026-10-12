/* linux/kexec.h -- Kestrel's Linux KPI: there is no crash kernel */
#ifndef _LKPI_LINUX_KEXEC_H
#define _LKPI_LINUX_KEXEC_H
#include <linux/types.h>
static inline bool kexec_crash_loaded(void) { return false; }
#endif
