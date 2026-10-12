/* asm/div64.h -- Kestrel's Linux KPI: do_div(n, base) divides n in place, returns the remainder */
#ifndef _LKPI_ASM_DIV64_H
#define _LKPI_ASM_DIV64_H
#include <linux/types.h>
#define do_div(n, base) ({ u32 __base = (base); u32 __rem = (u32)((u64)(n) % __base); (n) = (u64)(n) / __base; __rem; })
#endif
