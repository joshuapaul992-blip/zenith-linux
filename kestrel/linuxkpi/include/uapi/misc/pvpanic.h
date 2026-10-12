/* uapi/misc/pvpanic.h -- Kestrel's Linux KPI: the pvpanic device's event bits
 * (QEMU's pvpanic interface: docs/specs/pvpanic.rst) */
#ifndef _LKPI_UAPI_MISC_PVPANIC_H
#define _LKPI_UAPI_MISC_PVPANIC_H
#include <linux/const.h>
#define PVPANIC_PANICKED        _BITUL(0)       /* the guest panicked */
#define PVPANIC_CRASH_LOADED    _BITUL(1)       /* ... and a crash kernel takes over */
#define PVPANIC_SHUTDOWN        _BITUL(2)       /* the guest is powering off */
#endif
