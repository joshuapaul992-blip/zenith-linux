/* linux/device.h -- Kestrel's Linux KPI: the driver model (linuxkpi/src/device.c)
 *
 * Devices sit on buses, drivers register with a bus, and the bus's match
 * function pairs them; probe() runs in a thread that may sleep. Each
 * device keeps a list of managed resources (devres): everything a devm_
 * call acquires is released, newest first, when probe fails or the
 * driver is unbound. */
#ifndef _LKPI_LINUX_DEVICE_H
#define _LKPI_LINUX_DEVICE_H

#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/gfp_types.h>
#include <linux/sysfs.h>
#include <linux/pm.h>
#include <linux/ioport.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <linux/overflow.h>
#include <linux/device/driver.h>
#include <linux/device/bus.h>
#include <linux/device/class.h>
#include <linux/dev_printk.h>

struct device_node;
struct fwnode_handle;
struct device_dma_parameters { unsigned int max_segment_size; unsigned int min_align_mask; unsigned long segment_boundary_mask; };
struct device_type {
    const char *name;
    const struct attribute_group **groups;
    void (*release)(struct device *dev);
    const struct dev_pm_ops *pm;
};

struct device {
    struct kobject              kobj;
    struct device              *parent;
    const char                 *init_name;
    const struct device_type   *type;
    const struct bus_type      *bus;
    struct device_driver       *driver;
    void                       *platform_data;
    void                       *driver_data;
    struct dev_pm_info          power;
    const struct dev_pm_ops    *pm_domain;
    u64                        *dma_mask;
    u64                         coherent_dma_mask;
    u64                         bus_dma_limit;
    struct device_dma_parameters *dma_parms;
    struct device_node         *of_node;
    struct fwnode_handle       *fwnode;
    int                         numa_node;
    dev_t                       devt;
    u32                         id;
    const struct class         *class;
    const struct attribute_group **groups;
    void                      (*release)(struct device *dev);
    bool                        offline_disabled:1;
    bool                        offline:1;
    bool                        dma_coherent:1;
    /* the KPI's own */
    struct list_head            lkpi_devres;    /* managed resources, newest first */
    spinlock_t                  lkpi_devres_lock;
    struct list_head            lkpi_bus_node;  /* on its bus's device list */
    void                       *lkpi_devnode;   /* /dev node when devt is set */
    bool                        lkpi_added;
};

struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
    ssize_t (*store)(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
};
#define DEVICE_ATTR(_name, _mode, _show, _store) \
    struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RW(_name)   struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_ADMIN_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0600)
#define DEVICE_ATTR_RO(_name)   struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_ADMIN_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO_MODE(_name, 0400)
#define DEVICE_ATTR_WO(_name)   struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
static inline int device_create_file(struct device *d, const struct device_attribute *a) { return 0; }
static inline void device_remove_file(struct device *d, const struct device_attribute *a) { }
static inline int device_add_group(struct device *d, const struct attribute_group *g) { return 0; }
static inline void device_remove_group(struct device *d, const struct attribute_group *g) { }
static inline int device_add_groups(struct device *d, const struct attribute_group **g) { return 0; }
static inline void device_remove_groups(struct device *d, const struct attribute_group **g) { }
static inline int devm_device_add_group(struct device *d, const struct attribute_group *g) { return 0; }
static inline int devm_device_add_groups(struct device *d, const struct attribute_group **g) { return 0; }

static inline const char *dev_name(const struct device *dev)
{ return dev->init_name && !dev->kobj.name ? dev->init_name : dev->kobj.name ? dev->kobj.name : ""; }
__printf(2, 3) int dev_set_name(struct device *dev, const char *name, ...);
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }
static inline int dev_to_node(struct device *dev) { return -1; }
static inline void set_dev_node(struct device *dev, int node) { }
static inline struct device_node *dev_of_node(struct device *dev) { return NULL; }
static inline bool device_is_registered(struct device *dev) { return dev->lkpi_added; }
static inline void device_lock(struct device *dev) { }
static inline int device_trylock(struct device *dev) { return 1; }
static inline void device_unlock(struct device *dev) { }
static inline void device_lock_assert(struct device *dev) { }
const char *dev_driver_string(const struct device *dev);

void device_initialize(struct device *dev);
int  device_add(struct device *dev);
void device_del(struct device *dev);
int  device_register(struct device *dev);
void device_unregister(struct device *dev);
struct device *get_device(struct device *dev);
void put_device(struct device *dev);
int  device_attach(struct device *dev);
void device_release_driver(struct device *dev);
int  device_for_each_child(struct device *dev, void *data, int (*fn)(struct device *dev, void *data));
__printf(5, 6) struct device *device_create(const struct class *cls, struct device *parent, dev_t devt,
                                            void *drvdata, const char *fmt, ...);
void device_destroy(const struct class *cls, dev_t devt);

/* ---- managed resources --------------------------------------------------- */
typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);
void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name);
#define devres_alloc(release, size, gfp)        __devres_alloc_node(release, size, gfp, -1, #release)
#define devres_alloc_node(release, size, gfp, nid) __devres_alloc_node(release, size, gfp, nid, #release)
void devres_free(void *res);
void devres_add(struct device *dev, void *res);
void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int  devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int  devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
void lkpi_devres_release_all(struct device *dev);

__malloc void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
void *devm_krealloc(struct device *dev, void *ptr, size_t size, gfp_t gfp);
void devm_kfree(struct device *dev, const void *p);
static inline __malloc void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{ return devm_kmalloc(dev, size, gfp | __GFP_ZERO); }
static inline __malloc void *devm_kmalloc_array(struct device *dev, size_t n, size_t size, gfp_t gfp)
{ size_t b; return __builtin_mul_overflow(n, size, &b) ? NULL : devm_kmalloc(dev, b, gfp); }
static inline __malloc void *devm_kcalloc(struct device *dev, size_t n, size_t size, gfp_t gfp)
{ return devm_kmalloc_array(dev, n, size, gfp | __GFP_ZERO); }
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp);
const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp);
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp);
__printf(3, 4) char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...);
unsigned long devm_get_free_pages(struct device *dev, gfp_t gfp, unsigned int order);
void devm_free_pages(struct device *dev, unsigned long addr);

int  __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name);
#define devm_add_action(dev, action, data)      __devm_add_action(dev, action, data, #action)
static inline int __devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data, const char *name)
{
    int ret = __devm_add_action(dev, action, data, name);
    if (ret) action(data);
    return ret;
}
#define devm_add_action_or_reset(dev, action, data) __devm_add_action_or_reset(dev, action, data, #action)
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);
static inline int devm_remove_action_nowarn(struct device *dev, void (*action)(void *), void *data)
{ devm_remove_action(dev, action, data); return 0; }

#endif
