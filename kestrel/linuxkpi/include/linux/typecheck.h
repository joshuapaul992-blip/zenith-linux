/* linux/typecheck.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_TYPECHECK_H
#define _LKPI_LINUX_TYPECHECK_H
#define typecheck(T, x) ({ T __dummy; typeof(x) __dummy2; (void)(&__dummy == &__dummy2); 1; })
#define typecheck_fn(T, f) ({ typeof(T) __tmp = f; (void)__tmp; })
#endif
