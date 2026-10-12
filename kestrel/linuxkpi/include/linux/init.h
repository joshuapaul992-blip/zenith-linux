/* linux/init.h -- Kestrel's Linux KPI: initcalls
 *
 * module_init() and the *_initcall() macros put a record in the
 * .lkpi_initcall.<level> sections; linuxkpi_init() runs them by level,
 * in link order within a level, once PCI, MSI, the scheduler and the
 * root file system are up (see linker.ld and linuxkpi/src/module.c). */
#ifndef _LKPI_LINUX_INIT_H
#define _LKPI_LINUX_INIT_H

#include <linux/compiler.h>
#include <linux/types.h>

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "linuxkpi"
#endif

#define __init
#define __exit
#define __initdata
#define __initconst
#define __exitdata
#define __init_or_module
#define __meminit
#define __refdata
#define __ref
#define __devinit
#define __devexit
#define __exit_p(x)             x
#define __devexit_p(x)          x

typedef int (*initcall_t)(void);
typedef void (*exitcall_t)(void);

struct lkpi_initcall {
    initcall_t      fn;
    const char     *name;
    const char     *modname;
};

#define __lkpi_define_initcall(fn, level) \
    static const struct lkpi_initcall __UNIQUE_ID(initcall_##fn) __used \
        __section(".lkpi_initcall." #level) __aligned(8) = { fn, #fn, KBUILD_MODNAME }

#define pure_initcall(fn)       __lkpi_define_initcall(fn, 0)
#define core_initcall(fn)       __lkpi_define_initcall(fn, 1)
#define postcore_initcall(fn)   __lkpi_define_initcall(fn, 2)
#define arch_initcall(fn)       __lkpi_define_initcall(fn, 3)
#define subsys_initcall(fn)     __lkpi_define_initcall(fn, 4)
#define fs_initcall(fn)         __lkpi_define_initcall(fn, 5)
#define device_initcall(fn)     __lkpi_define_initcall(fn, 6)
#define late_initcall(fn)       __lkpi_define_initcall(fn, 7)
#define late_initcall_sync(fn)  __lkpi_define_initcall(fn, 7)
#define __initcall(fn)          device_initcall(fn)
/* nothing is ever unloaded: exit functions are kept but not called */
#define __exitcall(fn)          static exitcall_t __UNIQUE_ID(exitcall_##fn) __used = fn

#endif
