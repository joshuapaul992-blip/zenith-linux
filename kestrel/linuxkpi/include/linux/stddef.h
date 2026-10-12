/* linux/stddef.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_STDDEF_H
#define _LKPI_LINUX_STDDEF_H

#include <linux/compiler_types.h>

#undef NULL
#define NULL ((void *)0)

enum { false = 0, true = 1 };

#undef offsetof
#define offsetof(T, m)              __builtin_offsetof(T, m)
#define sizeof_field(T, m)          sizeof(((T *)0)->m)
#define offsetofend(T, m)           (offsetof(T, m) + sizeof_field(T, m))

/* A group of members that can also be addressed as one struct. */
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { struct { MEMBERS } ATTRS; struct TAG { MEMBERS } ATTRS NAME; }
#define struct_group(NAME, MEMBERS...)          __struct_group(, NAME, , MEMBERS)
#define struct_group_attr(NAME, ATTRS, MEMBERS...) __struct_group(, NAME, ATTRS, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) __struct_group(TAG, NAME, , MEMBERS)
#define DECLARE_FLEX_ARRAY(TYPE, NAME)  struct { struct { } __empty_##NAME; TYPE NAME[]; }

#endif
