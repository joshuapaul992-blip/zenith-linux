/* linux/compiler.h -- Kestrel's Linux KPI: branch hints, barriers, ONCE accessors */
#ifndef _LKPI_LINUX_COMPILER_H
#define _LKPI_LINUX_COMPILER_H

#include <linux/compiler_types.h>
#include <linux/stringify.h>

#define likely(x)               __builtin_expect(!!(x), 1)
#define unlikely(x)             __builtin_expect(!!(x), 0)
#define likely_notrace(x)       likely(x)
#define unlikely_notrace(x)     unlikely(x)
#define barrier()               __asm__ __volatile__("" ::: "memory")
#define barrier_data(p)         __asm__ __volatile__("" :: "r"(p) : "memory")
#define unreachable()           __builtin_unreachable()
#define RELOC_HIDE(p, off)      ((typeof(p))((unsigned long)(p) + (off)))
#define OPTIMIZER_HIDE_VAR(v)   __asm__ ("" : "=r"(v) : "0"(v))

#define ___LKPI_UNIQUE_ID(p, n) __PASTE(__PASTE(__UNIQUE_ID_, p), n)
#define __UNIQUE_ID(prefix)     ___LKPI_UNIQUE_ID(prefix, __COUNTER__)

/* A single, untorn access the compiler may not merge, repeat or drop. */
#define READ_ONCE(x)            (*(const volatile __unqual_scalar_typeof(x) *)&(x))
#define WRITE_ONCE(x, val)      do { *(volatile typeof(x) *)&(x) = (val); } while (0)
#define data_race(expr)         ({ __unqual_scalar_typeof(({ expr; })) __v = ({ expr; }); __v; })

/* &a[0] degrades to a pointer: a different type from the array */
#define __must_be_array(a)      (sizeof(char[1 - 2 * __same_type((a), &(a)[0])]) - 1)
#define __is_constexpr(x)       (sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

#define __annotate_jump_table
#define __ADDRESSABLE(sym)
#define prevent_tail_call_optimization() barrier()

#endif
