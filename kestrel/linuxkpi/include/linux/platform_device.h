/* linux/platform_device.h -- Kestrel's Linux KPI: the platform bus (linuxkpi/src/device.c)
 *
 * Devices that are not discovered by a bus: code registers them by name,
 * and a platform_driver with the same name (or an id_table entry) binds. */
#ifndef _LKPI_LINUX_PLATFORM_DEVICE_H
#define _LKPI_LINUX_PLATFORM_DEVICE_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#define PLATFORM_DEVID_NONE     (-1)
#define PLATFORM_DEVID_AUTO     (-2)

struct platform_device {
    const char                  *name;
    int                          id;
    bool                         id_auto;
    struct device                dev;
    u64                          platform_dma_mask;
    u32                          num_resources;
    struct resource             *resource;
    const struct platform_device_id *id_entry;
    const char                  *driver_override;
};
#define to_platform_device(x)   container_of((x), struct platform_device, dev)
#define platform_get_device_id(pdev)    ((pdev)->id_entry)

struct platform_driver {
    int  (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    int  (*suspend)(struct platform_device *, pm_message_t state);
    int  (*resume)(struct platform_device *);
    struct device_driver driver;
    const struct platform_device_id *id_table;
    bool prevent_deferred_probe;
    bool driver_managed_dma;
};
#define to_platform_driver(drv) container_of_const((drv), struct platform_driver, driver)

extern const struct bus_type platform_bus_type;

int  platform_device_register(struct platform_device *pdev);
void platform_device_unregister(struct platform_device *pdev);
struct platform_device *platform_device_alloc(const char *name, int id);
int  platform_device_add(struct platform_device *pdev);
void platform_device_del(struct platform_device *pdev);
void platform_device_put(struct platform_device *pdev);
int  platform_device_add_resources(struct platform_device *pdev, const struct resource *res, unsigned int num);
int  platform_device_add_data(struct platform_device *pdev, const void *data, size_t size);
struct platform_device *platform_device_register_simple(const char *name, int id,
                                                        const struct resource *res, unsigned int num);
struct platform_device *platform_device_register_data(struct device *parent, const char *name, int id,
                                                      const void *data, size_t size);

int  __platform_driver_register(struct platform_driver *drv, struct module *owner);
#define platform_driver_register(drv)   __platform_driver_register(drv, THIS_MODULE)
void platform_driver_unregister(struct platform_driver *drv);
#define module_platform_driver(__platform_driver) \
    module_driver(__platform_driver, platform_driver_register, platform_driver_unregister)
#define builtin_platform_driver(__platform_driver) \
    builtin_driver(__platform_driver, platform_driver_register)

struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num);
struct resource *platform_get_resource_byname(struct platform_device *dev, unsigned int type, const char *name);
int  platform_get_irq(struct platform_device *dev, unsigned int num);
int  platform_get_irq_optional(struct platform_device *dev, unsigned int num);
void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev, unsigned int index);
static inline void *platform_get_drvdata(const struct platform_device *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device *pdev, void *data) { dev_set_drvdata(&pdev->dev, data); }
static inline void *dev_get_platdata(const struct device *dev) { return dev->platform_data; }

#endif
