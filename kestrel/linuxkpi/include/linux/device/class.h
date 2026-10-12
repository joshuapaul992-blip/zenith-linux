/* linux/device/class.h -- Kestrel's Linux KPI: device classes (names only, no /sys/class yet) */
#ifndef _LKPI_LINUX_DEVICE_CLASS_H
#define _LKPI_LINUX_DEVICE_CLASS_H

#include <linux/types.h>
#include <linux/err.h>

struct device;
struct attribute_group;
struct kobj_uevent_env;

struct class {
    const char *name;
    const struct attribute_group **class_groups;
    const struct attribute_group **dev_groups;
    int  (*dev_uevent)(const struct device *dev, struct kobj_uevent_env *env);
    char *(*devnode)(const struct device *dev, umode_t *mode);
    void (*class_release)(const struct class *cls);
    void (*dev_release)(struct device *dev);
    const struct dev_pm_ops *pm;
};

static inline int class_register(const struct class *cls) { return 0; }
static inline void class_unregister(const struct class *cls) { }
struct class *class_create(const char *name);
void class_destroy(const struct class *cls);

#endif
