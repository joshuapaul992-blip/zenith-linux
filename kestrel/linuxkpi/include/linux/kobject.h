/* linux/kobject.h -- Kestrel's Linux KPI: just enough kobject for struct device (no sysfs tree) */
#ifndef _LKPI_LINUX_KOBJECT_H
#define _LKPI_LINUX_KOBJECT_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/sysfs.h>

struct kobject {
    const char         *name;
    struct kobject     *parent;
    struct kref         kref;
    struct list_head    entry;
};

static inline const char *kobject_name(const struct kobject *k) { return k->name; }
#define KOBJ_ADD        0
#define KOBJ_REMOVE     1
#define KOBJ_CHANGE     2
#define KOBJ_ONLINE     5
#define KOBJ_OFFLINE    6
static inline int kobject_uevent(struct kobject *k, int action) { return 0; }
static inline int kobject_uevent_env(struct kobject *k, int action, char *envp[]) { return 0; }

#endif
