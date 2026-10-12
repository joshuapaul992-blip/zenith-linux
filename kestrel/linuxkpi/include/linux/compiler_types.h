/* linux/compiler_types.h -- Kestrel's Linux KPI: annotations and attributes
 * (force-included into every Linux-side file, as Kbuild does) */
#ifndef _LKPI_LINUX_COMPILER_TYPES_H
#define _LKPI_LINUX_COMPILER_TYPES_H

#ifndef __ASSEMBLY__

/* sparse annotations: nothing to the compiler */
#define __user
#define __kernel
#define __iomem
#define __percpu
#define __rcu
#define __force
#define __nocast
#define __safe
#define __private
#define __bitwise
#define __must_hold(x)
#define __acquires(x)
#define __releases(x)
#define __acquire(x)            ((void)0)
#define __release(x)            ((void)0)
#define __cond_lock(x, c)       (c)
#define ACCESS_PRIVATE(p, member) ((p)->member)

/* attributes */
#define __always_inline         inline __attribute__((__always_inline__))
#define noinline                __attribute__((__noinline__))
#define __noinline              noinline
#define __packed                __attribute__((__packed__))
#define __aligned(x)            __attribute__((__aligned__(x)))
#define __section(s)            __attribute__((__section__(s)))
#define __used                  __attribute__((__used__))
#define __maybe_unused          __attribute__((__unused__))
#define __always_unused         __attribute__((__unused__))
#define __must_check            __attribute__((__warn_unused_result__))
#define __printf(a, b)          __attribute__((__format__(printf, a, b)))
#define __scanf(a, b)           __attribute__((__format__(scanf, a, b)))
#define __cold                  __attribute__((__cold__))
#define __hot                   __attribute__((__hot__))
#define __weak                  __attribute__((__weak__))
#define __pure                  __attribute__((__pure__))
#define __noreturn              __attribute__((__noreturn__))
#define __malloc                __attribute__((__malloc__))
#define __alias(s)              __attribute__((__alias__(#s)))
#define __attribute_const__     __attribute__((__const__))
#define __visible               __attribute__((__externally_visible__))
#define __deprecated
#define __alloc_size(x, ...)
#define __realloc_size(x, ...)
#define __assume_aligned(a, ...) __attribute__((__assume_aligned__(a, ## __VA_ARGS__)))
#define __counted_by(m)
#define __counted_by_le(m)
#define __counted_by_be(m)
#define __nonstring             __attribute__((__nonstring__))
#define __randomize_layout
#define __no_randomize_layout
#define randomized_struct_fields_start  struct {
#define randomized_struct_fields_end    };
#define __designated_init
#define __latent_entropy
#define __no_sanitize_address
#define __nocfi
#define __noscs
#define __cficanonical
#define __ro_after_init
#define __read_mostly
#define __cacheline_aligned     __aligned(64)
#define __cacheline_aligned_in_smp __aligned(64)
#define ____cacheline_aligned   __aligned(64)
#define ____cacheline_aligned_in_smp __aligned(64)
#define notrace                 __attribute__((__no_instrument_function__))
#define __naked                 __attribute__((__naked__)) notrace
#define __gnu_inline            __attribute__((__gnu_inline__))
#define __inline__              inline
#define __inline                inline
#define __no_kcsan
#define __no_sanitize_or_inline __always_inline
#define fallthrough             __attribute__((__fallthrough__))
#define __diag_push()
#define __diag_pop()
#define __diag_ignore(c, v, o, r)
#define __diag_ignore_all(o, r)

#define __same_type(a, b)       __builtin_types_compatible_p(typeof(a), typeof(b))
#define __native_word(t) \
    (sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))
#define __unqual_scalar_typeof(x) typeof(_Generic((x), \
    char: (char)0, unsigned char: (unsigned char)0, signed char: (signed char)0, \
    unsigned short: (unsigned short)0, signed short: (signed short)0, \
    unsigned int: (unsigned int)0, signed int: (signed int)0, \
    unsigned long: (unsigned long)0, signed long: (signed long)0, \
    unsigned long long: (unsigned long long)0, signed long long: (signed long long)0, \
    _Bool: (_Bool)0, default: (x)))

#define compiletime_assert(cond, msg) _Static_assert(cond, msg)
#define __compiletime_error(msg)

#endif /* __ASSEMBLY__ */
#endif
