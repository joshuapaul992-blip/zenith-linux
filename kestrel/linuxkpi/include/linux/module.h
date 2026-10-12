/* linux/module.h -- Kestrel's Linux KPI: modules are built in
 *
 * module_init() is an initcall (init.h); module information is kept in
 * the .lkpi_modinfo section and logged as each module initializes. */
#ifndef _LKPI_LINUX_MODULE_H
#define _LKPI_LINUX_MODULE_H

#include <linux/types.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/moduleparam.h>
#include <linux/stringify.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/kobject.h>

struct module { const char *name; };

#define module_init(fn)         device_initcall(fn)
#define module_exit(fn)         __exitcall(fn)
#define console_initcall(fn)    module_init(fn)

#define __lkpi_modinfo(tag, info) \
    static const char __UNIQUE_ID(modinfo)[] __used __section(".lkpi_modinfo") __aligned(1) = \
        KBUILD_MODNAME "." __stringify(tag) "=" info
#define MODULE_LICENSE(l)               __lkpi_modinfo(license, l)
#define MODULE_AUTHOR(a)                __lkpi_modinfo(author, a)
#define MODULE_DESCRIPTION(d)           __lkpi_modinfo(description, d)
#define MODULE_VERSION(v)               __lkpi_modinfo(version, v)
#define MODULE_ALIAS(a)                 __lkpi_modinfo(alias, a)
#define MODULE_FIRMWARE(f)              __lkpi_modinfo(firmware, f)
#define MODULE_SOFTDEP(s)               __lkpi_modinfo(softdep, s)
#define MODULE_INFO(tag, info)          __lkpi_modinfo(tag, info)
#define MODULE_IMPORT_NS(ns)            _Static_assert(1, "")
#define MODULE_DEVICE_TABLE(type, name) _Static_assert(sizeof(name) > 0, "")
#define MODULE_ALIAS_MISCDEV(minor)     _Static_assert(1, "")

static inline bool try_module_get(struct module *m) { return true; }
static inline void module_put(struct module *m) { }
static inline void __module_get(struct module *m) { }
#define module_name(m)                  KBUILD_MODNAME
#define symbol_get(x)                   (&(x))
#define symbol_put(x)                   do { } while (0)
#define request_module(...)             (-ENOENT)
#define request_module_nowait(...)      (-ENOENT)

/* module_driver(drv, register, unregister, extra args): the init/exit pair of most drivers */
#define module_driver(__driver, __register, __unregister, ...) \
    static int __init __driver##_init(void) { return __register(&(__driver), ##__VA_ARGS__); } \
    module_init(__driver##_init); \
    static void __exit __driver##_exit(void) { __unregister(&(__driver), ##__VA_ARGS__); } \
    module_exit(__driver##_exit)
#define builtin_driver(__driver, __register, ...) \
    static int __init __driver##_init(void) { return __register(&(__driver), ##__VA_ARGS__); } \
    device_initcall(__driver##_init)

#endif
