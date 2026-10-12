/* linux/stringify.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_STRINGIFY_H
#define _LKPI_LINUX_STRINGIFY_H
#define __stringify_1(x...)     #x
#define __stringify(x...)       __stringify_1(x)
#define ___PASTE(a, b)          a##b
#define __PASTE(a, b)           ___PASTE(a, b)
#define __CONCAT(a, b)          __PASTE(a, b)
#endif
