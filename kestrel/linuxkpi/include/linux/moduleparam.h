/* linux/moduleparam.h -- Kestrel's Linux KPI: module parameters from the kernel command line
 *
 * module_param(x, int, 0) in module "foo" is set by "foo.x=value" on the
 * Kestrel command line (linuxkpi/src/module.c), before the module's init. */
#ifndef _LKPI_LINUX_MODULEPARAM_H
#define _LKPI_LINUX_MODULEPARAM_H

#include <linux/init.h>
#include <linux/stringify.h>
#include <linux/types.h>

struct lkpi_param {
    const char     *name;               /* "module.param" */
    const char     *type;               /* int, uint, long, ulong, short, ushort, byte, bool, invbool, charp, hexint, string */
    void           *arg;
    unsigned int    len;                /* string: buffer size */
};

#define __lkpi_param(pname, var, ptype, plen) \
    static const struct lkpi_param __UNIQUE_ID(param_##pname) __used __section(".lkpi_param") __aligned(8) = \
        { KBUILD_MODNAME "." __stringify(pname), #ptype, (void *)&(var), (plen) }

#define module_param_named(name, value, type, perm)     __lkpi_param(name, value, type, 0)
#define module_param(name, type, perm)                  module_param_named(name, name, type, perm)
#define module_param_unsafe(name, type, perm)           module_param(name, type, perm)
#define module_param_named_unsafe(n, v, t, p)           module_param_named(n, v, t, p)
#define module_param_string(name, string, len, perm)    __lkpi_param(name, string, string, len)
/* arrays are not parsed yet: the default stays */
#define module_param_array(name, type, nump, perm)      __lkpi_param(name, name, array, 0)
#define module_param_array_named(name, arr, t, nump, p) __lkpi_param(name, arr, array, 0)
#define module_param_cb(name, ops, arg, perm)           _Static_assert(1, "")
#define core_param(name, var, type, perm)               module_param_named(name, var, type, perm)
#define MODULE_PARM_DESC(var, desc)                     _Static_assert(1, "")
#define __MODULE_STRING(x)                              __stringify(x)

struct kernel_param;
struct kernel_param_ops {
    unsigned int flags;
    int (*set)(const char *val, const struct kernel_param *kp);
    int (*get)(char *buffer, const struct kernel_param *kp);
    void (*free)(void *arg);
};

#endif
