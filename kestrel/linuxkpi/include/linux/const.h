/* linux/const.h -- Kestrel's Linux KPI */
#ifndef _LKPI_LINUX_CONST_H
#define _LKPI_LINUX_CONST_H
#define __AC(X, Y)      (X##Y)
#define _AC(X, Y)       __AC(X, Y)
#define _AT(T, X)       ((T)(X))
#define _UL(x)          (_AC(x, UL))
#define _ULL(x)         (_AC(x, ULL))
#define UL(x)           _UL(x)
#define ULL(x)          _ULL(x)
#define _BITUL(x)       (_UL(1) << (x))
#define _BITULL(x)      (_ULL(1) << (x))
#define __ALIGN_KERNEL_MASK(x, mask)  (((x) + (mask)) & ~(mask))
#define __ALIGN_KERNEL(x, a)          __ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
#define __KERNEL_DIV_ROUND_UP(n, d)   (((n) + (d) - 1) / (d))
#endif
