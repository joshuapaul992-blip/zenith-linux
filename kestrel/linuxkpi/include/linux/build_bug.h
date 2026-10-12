/* linux/build_bug.h -- Kestrel's Linux KPI: compile-time checks */
#ifndef _LKPI_LINUX_BUILD_BUG_H
#define _LKPI_LINUX_BUILD_BUG_H

#include <linux/compiler.h>

#define BUILD_BUG_ON_ZERO(e)        ((int)(sizeof(struct { int:(-!!(e)); })))
#define BUILD_BUG_ON_INVALID(e)     ((void)(sizeof((long)(e))))
#define BUILD_BUG_ON_MSG(cond, msg) _Static_assert(!(cond), msg)
#define BUILD_BUG_ON(cond)          BUILD_BUG_ON_MSG(cond, "BUILD_BUG_ON failed: " #cond)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))
#define BUILD_BUG()                 BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")
#define static_assert(expr, ...)    __lkpi_static_assert(expr, ##__VA_ARGS__, #expr)
#define __lkpi_static_assert(expr, msg, ...) _Static_assert(expr, msg)

#endif
