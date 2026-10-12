/* linux/container_of.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_CONTAINER_OF_H
#define _LKPI_LINUX_CONTAINER_OF_H

#include <linux/build_bug.h>
#include <linux/stddef.h>

/* The structure of type T whose member m is at ptr. */
#define container_of(ptr, T, m) ({ \
        void *__mptr = (void *)(ptr); \
        _Static_assert(__same_type(*(ptr), ((T *)0)->m) || __same_type(*(ptr), void), \
                       "container_of: pointer type mismatch"); \
        ((T *)(__mptr - offsetof(T, m))); })
#define container_of_const(ptr, T, m) container_of(ptr, T, m)

#endif
