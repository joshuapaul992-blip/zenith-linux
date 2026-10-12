/* linux/device/driver.h -- Kestrel's Linux KPI: struct device_driver */
#ifndef _LKPI_LINUX_DEVICE_DRIVER_H
#define _LKPI_LINUX_DEVICE_DRIVER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/pm.h>

struct device;
struct bus_type;
struct module;
struct of_device_id;
struct acpi_device_id;
struct attribute_group;

enum probe_type { PROBE_DEFAULT_STRATEGY, PROBE_PREFER_ASYNCHRONOUS, PROBE_FORCE_SYNCHRONOUS };

struct device_driver {
    const char                  *name;
    const struct bus_type       *bus;
    struct module               *owner;
    const char                  *mod_name;
    bool                         suppress_bind_attrs;
    enum probe_type              probe_type;
    const struct of_device_id   *of_match_table;
    const struct acpi_device_id *acpi_match_table;
    int  (*probe)(struct device *dev);
    void (*sync_state)(struct device *dev);
    int  (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    int  (*suspend)(struct device *dev, pm_message_t state);
    int  (*resume)(struct device *dev);
    const struct attribute_group **groups;
    const struct attribute_group **dev_groups;
    const struct dev_pm_ops     *pm;
    void (*coredump)(struct device *dev);
    struct list_head             lkpi_bus_node;     /* on its bus's driver list */
};

int  driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);
struct device *driver_find_device(const struct device_driver *drv, struct device *start, const void *data,
                                  int (*match)(struct device *dev, const void *data));
int driver_for_each_device(struct device_driver *drv, struct device *start, void *data,
                           int (*fn)(struct device *dev, void *data));

#endif
