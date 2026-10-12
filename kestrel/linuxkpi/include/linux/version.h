/* linux/version.h -- Kestrel's Linux KPI: the Linux version the KPI follows */
#ifndef _LKPI_LINUX_VERSION_H
#define _LKPI_LINUX_VERSION_H
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE      KERNEL_VERSION(6, 12, 0)
#define LINUX_VERSION_MAJOR     6
#define LINUX_VERSION_PATCHLEVEL 12
#define LINUX_VERSION_SUBLEVEL  0
#endif
