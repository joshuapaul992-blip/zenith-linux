/* linux/device/bus.h -- Kestrel's Linux KPI: struct bus_type */
#ifndef _LKPI_LINUX_DEVICE_BUS_H
#define _LKPI_LINUX_DEVICE_BUS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/pm.h>

struct device;
struct device_driver;
struct attribute_group;
struct kobj_uevent_env;

struct bus_type {
    const char *name;
    const char *dev_name;
    const struct attribute_group **bus_groups;
    const struct attribute_group **dev_groups;
    const struct attribute_group **drv_groups;
    int  (*match)(struct device *dev, const struct device_driver *drv);
    int  (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
    int  (*probe)(struct device *dev);
    void (*sync_state)(struct device *dev);
    void (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    const struct dev_pm_ops *pm;
    bool need_parent_lock;
};

int  bus_register(const struct bus_type *bus);
void bus_unregister(const struct bus_type *bus);
int  bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
                      int (*fn)(struct device *dev, void *data));
struct device *bus_find_device(const struct bus_type *bus, struct device *start, const void *data,
                               int (*match)(struct device *dev, const void *data));
int  bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
                      int (*fn)(struct device_driver *drv, void *data));

#endif
