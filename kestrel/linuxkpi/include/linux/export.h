/* linux/export.h -- Kestrel's Linux KPI: everything is linked into one kernel, exports are no-ops */
#ifndef _LKPI_LINUX_EXPORT_H
#define _LKPI_LINUX_EXPORT_H
#define EXPORT_SYMBOL(sym)              _Static_assert(1, "")
#define EXPORT_SYMBOL_GPL(sym)          _Static_assert(1, "")
#define EXPORT_SYMBOL_NS(sym, ns)       _Static_assert(1, "")
#define EXPORT_SYMBOL_NS_GPL(sym, ns)   _Static_assert(1, "")
#define EXPORT_SYMBOL_FOR_TESTS_ONLY(sym) _Static_assert(1, "")
#define EXPORT_SYMBOL_IF_KUNIT(sym)     _Static_assert(1, "")
#define THIS_MODULE                     ((struct module *)0)
#endif
