/* linuxkpi/src/device.c -- the driver model: buses, devices, drivers, devres
 *
 * Each registered bus keeps a list of its devices and its drivers. Adding
 * a device tries every driver of its bus; registering a driver tries
 * every unbound device: the bus's match() decides, then probe() runs in
 * the calling thread. One recursive lock serialises binding (a probe may
 * register further devices or drivers). A probe that returns
 * -EPROBE_DEFER is retried whenever another driver binds, and once more
 * when the boot initcalls have run.
 *
 * Managed resources (devres) are a list per device, newest first; all of
 * them are released when probe fails or the driver is unbound.
 *
 * Also here: the platform bus and device classes. There is no sysfs:
 * attribute groups are accepted and ignored. */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/printk.h>
#include "internal.h"

struct lkpi_bus {
    const struct bus_type  *bus;
    struct list_head        devices;
    struct list_head        drivers;
    struct list_head        node;
};

static LIST_HEAD(buses);
static LIST_HEAD(deferred);             /* struct deferred_dev: probes to retry */
static LIST_HEAD(classless);            /* devices without a bus (device_create) */

struct deferred_dev { struct list_head node; struct device *dev; };

/* ---- the binding lock: a mutex that its holder may take again --------------- */
static DEFINE_MUTEX(bind_mutex);
static struct task_struct *bind_owner;
static int bind_depth;

static void bind_lock(void)
{
    if (READ_ONCE(bind_owner) == current) { bind_depth++; return; }
    mutex_lock(&bind_mutex);
    bind_owner = current;
    bind_depth = 1;
}

static void bind_unlock(void)
{
    if (--bind_depth) return;
    bind_owner = NULL;
    mutex_unlock(&bind_mutex);
}

static struct lkpi_bus *find_bus(const struct bus_type *bus)
{
    struct lkpi_bus *b;
    list_for_each_entry(b, &buses, node)
        if (b->bus == bus) return b;
    return NULL;
}

int bus_register(const struct bus_type *bus)
{
    bind_lock();
    if (find_bus(bus)) { bind_unlock(); return -EEXIST; }
    struct lkpi_bus *b = kzalloc(sizeof *b, GFP_KERNEL);
    if (!b) { bind_unlock(); return -ENOMEM; }
    b->bus = bus;
    INIT_LIST_HEAD(&b->devices);
    INIT_LIST_HEAD(&b->drivers);
    list_add_tail(&b->node, &buses);
    bind_unlock();
    return 0;
}

int lkpi_bus_add(const struct bus_type *bus) { return bus_register(bus); }

void bus_unregister(const struct bus_type *bus)
{
    bind_lock();
    struct lkpi_bus *b = find_bus(bus);
    if (b && list_empty(&b->devices) && list_empty(&b->drivers)) {
        list_del(&b->node);
        kfree(b);
    }
    bind_unlock();
}

/* ---- names and references ------------------------------------------------------ */
int dev_set_name(struct device *dev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *name = kvasprintf(GFP_KERNEL, fmt, ap);
    va_end(ap);
    if (!name) return -ENOMEM;
    kfree(dev->kobj.name);
    dev->kobj.name = name;
    return 0;
}

void device_initialize(struct device *dev)
{
    kref_init(&dev->kobj.kref);
    INIT_LIST_HEAD(&dev->kobj.entry);
    INIT_LIST_HEAD(&dev->lkpi_devres);
    spin_lock_init(&dev->lkpi_devres_lock);
    INIT_LIST_HEAD(&dev->lkpi_bus_node);
    dev->numa_node = -1;
}

static void device_release(struct kref *kref)
{
    struct device *dev = container_of(kref, struct device, kobj.kref);
    const char *name = dev->kobj.name;
    lkpi_devres_release_all(dev);
    if (dev->release) dev->release(dev);
    else if (dev->type && dev->type->release) dev->type->release(dev);
    else if (dev->class && dev->class->dev_release) dev->class->dev_release(dev);
    else printk(KERN_WARNING "linuxkpi: device '%s' has no release() function\n", name ? name : "?");
    kfree(name);
}

struct device *get_device(struct device *dev)
{
    if (dev) kref_get(&dev->kobj.kref);
    return dev;
}

void put_device(struct device *dev)
{
    if (dev) kref_put(&dev->kobj.kref, device_release);
}

/* ---- binding -------------------------------------------------------------------- */
static void defer(struct device *dev)
{
    struct deferred_dev *d;
    list_for_each_entry(d, &deferred, node)
        if (d->dev == dev) return;
    d = kzalloc(sizeof *d, GFP_KERNEL);
    if (!d) return;
    d->dev = get_device(dev);
    list_add_tail(&d->node, &deferred);
}

static void undefer(struct device *dev)
{
    struct deferred_dev *d, *n;
    list_for_each_entry_safe(d, n, &deferred, node) {
        if (d->dev == dev) {
            list_del(&d->node);
            put_device(dev);
            kfree(d);
        }
    }
}

/* bind_lock held: 1 bound, 0 no match, < 0 the probe failed */
static int try_bind(struct device *dev, struct device_driver *drv)
{
    const struct bus_type *bus = dev->bus;
    if (bus->match && !bus->match(dev, drv)) return 0;

    dev->driver = drv;
    int ret = bus->probe ? bus->probe(dev) : drv->probe ? drv->probe(dev) : 0;
    if (ret) {
        lkpi_devres_release_all(dev);
        dev->driver = NULL;
        dev_set_drvdata(dev, NULL);
        if (ret == -EPROBE_DEFER) {
            printk(KERN_INFO "linuxkpi: %s %s: probe deferred\n", drv->name, dev_name(dev));
            defer(dev);
        } else if (ret != -ENODEV && ret != -ENXIO) {
            printk(KERN_WARNING "linuxkpi: %s: probe of %s failed with error %d\n", drv->name, dev_name(dev), ret);
        }
        return ret;
    }
    undefer(dev);
    printk(KERN_INFO "linuxkpi: driver %s bound to %s %s\n", drv->name, bus->name, dev_name(dev));
    return 1;
}

static int attach_locked(struct device *dev)
{
    struct lkpi_bus *b = dev->bus ? find_bus(dev->bus) : NULL;
    struct device_driver *drv;
    if (!b || dev->driver) return dev->driver ? 1 : 0;
    list_for_each_entry(drv, &b->drivers, lkpi_bus_node) {
        int r = try_bind(dev, drv);
        if (r == 1) return 1;
        if (r == -EPROBE_DEFER) return 0;
    }
    return 0;
}

int device_attach(struct device *dev)
{
    bind_lock();
    int r = attach_locked(dev);
    bind_unlock();
    if (r == 1) lkpi_deferred_probe_flush();
    return r;
}

/* retry deferred probes until a round binds nothing more */
void lkpi_deferred_probe_flush(void)
{
    bool progress;
    bind_lock();
    if (bind_depth > 1) { bind_unlock(); return; }      /* the outermost caller does it */
    do {
        LIST_HEAD(round);
        list_splice_init(&deferred, &round);
        progress = false;
        while (!list_empty(&round)) {
            struct deferred_dev *d = list_first_entry(&round, struct deferred_dev, node);
            struct device *dev = d->dev;                /* the reference moves to us */
            list_del(&d->node);
            kfree(d);
            if (attach_locked(dev) == 1) progress = true;
            put_device(dev);
        }
    } while (progress && !list_empty(&deferred));
    bind_unlock();
}

void device_release_driver(struct device *dev)
{
    bind_lock();
    struct device_driver *drv = dev->driver;
    if (drv) {
        if (dev->bus && dev->bus->remove) dev->bus->remove(dev);
        else if (drv->remove) drv->remove(dev);
        lkpi_devres_release_all(dev);
        dev->driver = NULL;
        dev_set_drvdata(dev, NULL);
        printk(KERN_INFO "linuxkpi: driver %s unbound from %s\n", drv->name, dev_name(dev));
    }
    bind_unlock();
}

/* ---- devices ------------------------------------------------------------------------ */
int device_add(struct device *dev)
{
    if (!dev->kobj.name) {
        if (dev->init_name) dev_set_name(dev, "%s", dev->init_name);
        else if (dev->bus && dev->bus->dev_name) dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);
        else return -EINVAL;
    }
    get_device(dev->parent);
    bind_lock();
    if (dev->bus) {
        struct lkpi_bus *b = find_bus(dev->bus);
        if (!b) {
            bind_unlock();
            put_device(dev->parent);
            printk(KERN_ERR "linuxkpi: device %s on unregistered bus %s\n", dev_name(dev), dev->bus->name);
            return -EINVAL;
        }
        list_add_tail(&dev->lkpi_bus_node, &b->devices);
    } else {
        list_add_tail(&dev->lkpi_bus_node, &classless);
    }
    dev->lkpi_added = true;
    if (dev->devt) dev->lkpi_devnode = lkpi_devnode_add(dev);
    int r = dev->bus ? attach_locked(dev) : 0;
    bind_unlock();
    if (r == 1) lkpi_deferred_probe_flush();
    return 0;
}

void device_del(struct device *dev)
{
    device_release_driver(dev);
    bind_lock();
    undefer(dev);
    if (dev->lkpi_devnode) lkpi_devnode_remove(dev);
    if (dev->lkpi_added) list_del_init(&dev->lkpi_bus_node);
    dev->lkpi_added = false;
    bind_unlock();
    put_device(dev->parent);
}

int device_register(struct device *dev)
{
    device_initialize(dev);
    return device_add(dev);
}

void device_unregister(struct device *dev)
{
    device_del(dev);
    put_device(dev);
}

int device_for_each_child(struct device *parent, void *data, int (*fn)(struct device *dev, void *data))
{
    struct lkpi_bus *b;
    struct device *dev;
    int r = 0;
    bind_lock();
    list_for_each_entry(b, &buses, node)
        list_for_each_entry(dev, &b->devices, lkpi_bus_node)
            if (dev->parent == parent && (r = fn(dev, data))) goto out;
    list_for_each_entry(dev, &classless, lkpi_bus_node)
        if (dev->parent == parent && (r = fn(dev, data))) goto out;
out:
    bind_unlock();
    return r;
}

/* ---- drivers --------------------------------------------------------------------------- */
int driver_register(struct device_driver *drv)
{
    struct lkpi_bus *b;
    struct device *dev;
    bind_lock();
    if (!drv->bus || !(b = find_bus(drv->bus))) {
        bind_unlock();
        printk(KERN_ERR "linuxkpi: driver %s: no such bus\n", drv->name);
        return -EINVAL;
    }
    list_add_tail(&drv->lkpi_bus_node, &b->drivers);
    bool bound = false;
    list_for_each_entry(dev, &b->devices, lkpi_bus_node)
        if (!dev->driver && try_bind(dev, drv) == 1) bound = true;
    bind_unlock();
    if (bound) lkpi_deferred_probe_flush();
    return 0;
}

void driver_unregister(struct device_driver *drv)
{
    struct lkpi_bus *b;
    struct device *dev;
    bind_lock();
    if (drv->bus && (b = find_bus(drv->bus))) {
        list_for_each_entry(dev, &b->devices, lkpi_bus_node)
            if (dev->driver == drv) device_release_driver(dev);
        list_del(&drv->lkpi_bus_node);
    }
    bind_unlock();
}

int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data,
                     int (*fn)(struct device *dev, void *data))
{
    struct device *dev;
    int r = 0;
    bind_lock();
    struct lkpi_bus *b = find_bus(bus);
    bool started = !start;
    if (b) {
        list_for_each_entry(dev, &b->devices, lkpi_bus_node) {
            if (!started) { started = dev == start; continue; }
            if ((r = fn(dev, data))) break;
        }
    }
    bind_unlock();
    return r;
}

struct device *bus_find_device(const struct bus_type *bus, struct device *start, const void *data,
                               int (*match)(struct device *dev, const void *data))
{
    struct device *dev, *found = NULL;
    bind_lock();
    struct lkpi_bus *b = find_bus(bus);
    bool started = !start;
    if (b) {
        list_for_each_entry(dev, &b->devices, lkpi_bus_node) {
            if (!started) { started = dev == start; continue; }
            if (match(dev, data)) { found = get_device(dev); break; }
        }
    }
    bind_unlock();
    return found;
}

int bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
                     int (*fn)(struct device_driver *drv, void *data))
{
    struct device_driver *drv;
    int r = 0;
    bind_lock();
    struct lkpi_bus *b = find_bus(bus);
    bool started = !start;
    if (b) {
        list_for_each_entry(drv, &b->drivers, lkpi_bus_node) {
            if (!started) { started = drv == start; continue; }
            if ((r = fn(drv, data))) break;
        }
    }
    bind_unlock();
    return r;
}

struct device *driver_find_device(const struct device_driver *drv, struct device *start, const void *data,
                                  int (*match)(struct device *dev, const void *data))
{
    struct device *dev, *found = NULL;
    bind_lock();
    struct lkpi_bus *b = drv->bus ? find_bus(drv->bus) : NULL;
    bool started = !start;
    if (b) {
        list_for_each_entry(dev, &b->devices, lkpi_bus_node) {
            if (!started) { started = dev == start; continue; }
            if (dev->driver == drv && match(dev, data)) { found = get_device(dev); break; }
        }
    }
    bind_unlock();
    return found;
}

int driver_for_each_device(struct device_driver *drv, struct device *start, void *data,
                           int (*fn)(struct device *dev, void *data))
{
    struct device *dev;
    int r = 0;
    bind_lock();
    struct lkpi_bus *b = drv->bus ? find_bus(drv->bus) : NULL;
    bool started = !start;
    if (b) {
        list_for_each_entry(dev, &b->devices, lkpi_bus_node) {
            if (!started) { started = dev == start; continue; }
            if (dev->driver == drv && (r = fn(dev, data))) break;
        }
    }
    bind_unlock();
    return r;
}

/* ---- classes and device_create ----------------------------------------------------------- */
struct class *class_create(const char *name)
{
    struct class *cls = kzalloc(sizeof *cls, GFP_KERNEL);
    if (!cls) return ERR_PTR(-ENOMEM);
    cls->name = name;
    return cls;
}

void class_destroy(const struct class *cls)
{
    if (!IS_ERR_OR_NULL(cls)) kfree(cls);
}

static void device_create_release(struct device *dev) { kfree(dev); }

struct device *device_create(const struct class *cls, struct device *parent, dev_t devt,
                             void *drvdata, const char *fmt, ...)
{
    struct device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
    if (!dev) return ERR_PTR(-ENOMEM);
    device_initialize(dev);
    dev->class = cls;
    dev->parent = parent;
    dev->devt = devt;
    dev->release = device_create_release;
    dev_set_drvdata(dev, drvdata);
    va_list ap;
    va_start(ap, fmt);
    char *name = kvasprintf(GFP_KERNEL, fmt, ap);
    va_end(ap);
    if (!name) { put_device(dev); return ERR_PTR(-ENOMEM); }
    dev->kobj.name = name;
    int r = device_add(dev);
    if (r) { put_device(dev); return ERR_PTR(r); }
    return dev;
}

void device_destroy(const struct class *cls, dev_t devt)
{
    struct device *dev, *found = NULL;
    bind_lock();
    list_for_each_entry(dev, &classless, lkpi_bus_node)
        if (dev->class == cls && dev->devt == devt) { found = dev; break; }
    bind_unlock();
    if (found) device_unregister(found);
}

/* ---- managed resources ------------------------------------------------------------------- */
struct devres {
    struct list_head    node;
    dr_release_t        release;
    const char         *name;
    size_t              size;
    u8                  data[] __aligned(ARCH_KMALLOC_MINALIGN);
};

static struct devres *to_devres(void *res) { return container_of(res, struct devres, data); }

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name)
{
    struct devres *dr = kmalloc(sizeof *dr + size, gfp);
    if (!dr) return NULL;
    memset(dr, 0, sizeof *dr);
    INIT_LIST_HEAD(&dr->node);
    dr->release = release;
    dr->name = name;
    dr->size = size;
    return dr->data;
}

void devres_free(void *res)
{
    if (res) kfree(to_devres(res));
}

void devres_add(struct device *dev, void *res)
{
    unsigned long flags;
    struct devres *dr = to_devres(res);
    spin_lock_irqsave(&dev->lkpi_devres_lock, flags);
    list_add(&dr->node, &dev->lkpi_devres);
    spin_unlock_irqrestore(&dev->lkpi_devres_lock, flags);
}

static struct devres *find_locked(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    struct devres *dr;
    list_for_each_entry(dr, &dev->lkpi_devres, node) {
        if (dr->release != release) continue;
        if (match && !match(dev, dr->data, match_data)) continue;
        return dr;
    }
    return NULL;
}

void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    unsigned long flags;
    spin_lock_irqsave(&dev->lkpi_devres_lock, flags);
    struct devres *dr = find_locked(dev, release, match, match_data);
    spin_unlock_irqrestore(&dev->lkpi_devres_lock, flags);
    return dr ? dr->data : NULL;
}

static void *devres_remove(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    unsigned long flags;
    spin_lock_irqsave(&dev->lkpi_devres_lock, flags);
    struct devres *dr = find_locked(dev, release, match, match_data);
    if (dr) list_del_init(&dr->node);
    spin_unlock_irqrestore(&dev->lkpi_devres_lock, flags);
    return dr ? dr->data : NULL;
}

int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    void *res = devres_remove(dev, release, match, match_data);
    if (!res) return -ENOENT;
    devres_free(res);
    return 0;
}

int devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data)
{
    void *res = devres_remove(dev, release, match, match_data);
    if (!res) return -ENOENT;
    release(dev, res);
    devres_free(res);
    return 0;
}

void lkpi_devres_release_all(struct device *dev)
{
    if (!dev->lkpi_devres.next) return;                 /* never initialized */
    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&dev->lkpi_devres_lock, flags);
        struct devres *dr = list_first_entry_or_null(&dev->lkpi_devres, struct devres, node);
        if (dr) list_del_init(&dr->node);
        spin_unlock_irqrestore(&dev->lkpi_devres_lock, flags);
        if (!dr) break;
        if (dr->release) dr->release(dev, dr->data);
        kfree(dr);
    }
}

/* devm_kmalloc: the resource is the memory itself */
static void devm_kmalloc_release(struct device *dev, void *res) { }
static int devm_kmalloc_match(struct device *dev, void *res, void *data) { return res == data; }

void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
    if (!size) return ZERO_SIZE_PTR;
    void *p = __devres_alloc_node(devm_kmalloc_release, size, gfp, -1, "devm_kmalloc");
    if (!p) return NULL;
    if (gfp & __GFP_ZERO) memset(p, 0, size);
    devres_add(dev, p);
    return p;
}

void devm_kfree(struct device *dev, const void *p)
{
    if (ZERO_OR_NULL_PTR(p)) return;
    if (devres_destroy(dev, devm_kmalloc_release, devm_kmalloc_match, (void *)p))
        printk(KERN_WARNING "linuxkpi: devm_kfree of memory %s did not allocate\n", dev_name(dev));
}

void *devm_krealloc(struct device *dev, void *ptr, size_t size, gfp_t gfp)
{
    if (ZERO_OR_NULL_PTR(ptr)) return devm_kmalloc(dev, size, gfp);
    if (!size) { devm_kfree(dev, ptr); return ZERO_SIZE_PTR; }
    size_t old = to_devres(ptr)->size;
    if (size <= old) return ptr;
    void *n = devm_kmalloc(dev, size, gfp & ~__GFP_ZERO);
    if (!n) return NULL;
    memcpy(n, ptr, old);
    if (gfp & __GFP_ZERO) memset((u8 *)n + old, 0, size - old);
    devm_kfree(dev, ptr);
    return n;
}

char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = devm_kmalloc(dev, n, gfp);
    if (p) memcpy(p, s, n);
    return p;
}

const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp) { return devm_kstrdup(dev, s, gfp); }

void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
    void *p = devm_kmalloc(dev, len, gfp);
    if (p) memcpy(p, src, len);
    return p;
}

char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *p = devm_kmalloc(dev, (size_t)len + 1, gfp);
    if (p) vsnprintf(p, (size_t)len + 1, fmt, ap);
    va_end(ap);
    return p;
}

struct pages_devres { unsigned long addr; unsigned int order; };

static void devm_pages_release(struct device *dev, void *res)
{
    struct pages_devres *r = res;
    free_pages(r->addr, r->order);
}

static int devm_pages_match(struct device *dev, void *res, void *data)
{
    return ((struct pages_devres *)res)->addr == *(unsigned long *)data;
}

unsigned long devm_get_free_pages(struct device *dev, gfp_t gfp, unsigned int order)
{
    struct pages_devres *r = devres_alloc(devm_pages_release, sizeof *r, GFP_KERNEL);
    if (!r) return 0;
    unsigned long addr = __get_free_pages(gfp, order);
    if (!addr) { devres_free(r); return 0; }
    r->addr = addr;
    r->order = order;
    devres_add(dev, r);
    return addr;
}

void devm_free_pages(struct device *dev, unsigned long addr)
{
    devres_release(dev, devm_pages_release, devm_pages_match, &addr);
}

struct action_devres { void (*action)(void *); void *data; };

static void devm_action_release(struct device *dev, void *res)
{
    struct action_devres *a = res;
    a->action(a->data);
}

static int devm_action_match(struct device *dev, void *res, void *p)
{
    struct action_devres *a = res, *m = p;
    return a->action == m->action && a->data == m->data;
}

int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name)
{
    struct action_devres *a = __devres_alloc_node(devm_action_release, sizeof *a, GFP_KERNEL, -1, name);
    if (!a) return -ENOMEM;
    a->action = action;
    a->data = data;
    devres_add(dev, a);
    return 0;
}

void devm_remove_action(struct device *dev, void (*action)(void *), void *data)
{
    struct action_devres m = { action, data };
    devres_destroy(dev, devm_action_release, devm_action_match, &m);
}

void devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
    struct action_devres m = { action, data };
    devres_release(dev, devm_action_release, devm_action_match, &m);
}

/* ---- the platform bus -------------------------------------------------------------------- */
static const struct platform_device_id *platform_match_id(const struct platform_device_id *id,
                                                          struct platform_device *pdev)
{
    for (; id->name[0]; id++)
        if (!strcmp(pdev->name, id->name)) return id;
    return NULL;
}

static int platform_match(struct device *dev, const struct device_driver *drv)
{
    struct platform_device *pdev = to_platform_device(dev);
    const struct platform_driver *pdrv = to_platform_driver(drv);
    if (pdev->driver_override) return !strcmp(pdev->driver_override, drv->name);
    if (pdrv->id_table) {
        const struct platform_device_id *id = platform_match_id(pdrv->id_table, pdev);
        if (id) { pdev->id_entry = id; return 1; }
        return 0;
    }
    return !strcmp(pdev->name, drv->name);
}

static int platform_probe(struct device *dev)
{
    struct platform_driver *pdrv = (struct platform_driver *)to_platform_driver(dev->driver);
    return pdrv->probe ? pdrv->probe(to_platform_device(dev)) : 0;
}

static void platform_remove(struct device *dev)
{
    struct platform_driver *pdrv = (struct platform_driver *)to_platform_driver(dev->driver);
    if (pdrv->remove) pdrv->remove(to_platform_device(dev));
}

const struct bus_type platform_bus_type = {
    .name = "platform",
    .match = platform_match,
    .probe = platform_probe,
    .remove = platform_remove,
};

static DEFINE_IDA(platform_auto_ida);

struct platform_object {
    struct platform_device pdev;
    char name[];
};

static void platform_device_release(struct device *dev)
{
    struct platform_object *pa = container_of(dev, struct platform_object, pdev.dev);
    kfree(pa->pdev.dev.platform_data);
    kfree(pa->pdev.resource);
    kfree(pa);
}

struct platform_device *platform_device_alloc(const char *name, int id)
{
    struct platform_object *pa = kzalloc(sizeof *pa + strlen(name) + 1, GFP_KERNEL);
    if (!pa) return NULL;
    strcpy(pa->name, name);
    pa->pdev.name = pa->name;
    pa->pdev.id = id;
    device_initialize(&pa->pdev.dev);
    pa->pdev.dev.release = platform_device_release;
    /* as Linux: platform devices can do 32-bit DMA unless told otherwise */
    pa->pdev.platform_dma_mask = DMA_BIT_MASK(32);
    pa->pdev.dev.dma_mask = &pa->pdev.platform_dma_mask;
    pa->pdev.dev.coherent_dma_mask = DMA_BIT_MASK(32);
    return &pa->pdev;
}

int platform_device_add(struct platform_device *pdev)
{
    pdev->dev.bus = &platform_bus_type;
    if (pdev->id == PLATFORM_DEVID_AUTO) {
        int id = ida_alloc(&platform_auto_ida, GFP_KERNEL);
        if (id < 0) return id;
        pdev->id = id;
        pdev->id_auto = true;
        dev_set_name(&pdev->dev, "%s.%d.auto", pdev->name, id);
    } else if (pdev->id == PLATFORM_DEVID_NONE) {
        dev_set_name(&pdev->dev, "%s", pdev->name);
    } else {
        dev_set_name(&pdev->dev, "%s.%d", pdev->name, pdev->id);
    }
    return device_add(&pdev->dev);
}

void platform_device_del(struct platform_device *pdev)
{
    if (IS_ERR_OR_NULL(pdev)) return;
    device_del(&pdev->dev);
    if (pdev->id_auto) {
        ida_free(&platform_auto_ida, (unsigned)pdev->id);
        pdev->id = PLATFORM_DEVID_AUTO;
    }
}

void platform_device_put(struct platform_device *pdev)
{
    if (!IS_ERR_OR_NULL(pdev)) put_device(&pdev->dev);
}

int platform_device_register(struct platform_device *pdev)
{
    device_initialize(&pdev->dev);
    return platform_device_add(pdev);
}

void platform_device_unregister(struct platform_device *pdev)
{
    platform_device_del(pdev);
    platform_device_put(pdev);
}

int platform_device_add_resources(struct platform_device *pdev, const struct resource *res, unsigned int num)
{
    struct resource *r = NULL;
    if (res) {
        r = kmemdup(res, sizeof *res * num, GFP_KERNEL);
        if (!r) return -ENOMEM;
    }
    kfree(pdev->resource);
    pdev->resource = r;
    pdev->num_resources = num;
    return 0;
}

int platform_device_add_data(struct platform_device *pdev, const void *data, size_t size)
{
    void *d = NULL;
    if (data) {
        d = kmemdup(data, size, GFP_KERNEL);
        if (!d) return -ENOMEM;
    }
    kfree(pdev->dev.platform_data);
    pdev->dev.platform_data = d;
    return 0;
}

static struct platform_device *register_full(struct device *parent, const char *name, int id,
                                             const struct resource *res, unsigned int num,
                                             const void *data, size_t size)
{
    struct platform_device *pdev = platform_device_alloc(name, id);
    if (!pdev) return ERR_PTR(-ENOMEM);
    pdev->dev.parent = parent;
    int r = platform_device_add_resources(pdev, res, num);
    if (!r) r = platform_device_add_data(pdev, data, size);
    if (!r) r = platform_device_add(pdev);
    if (r) { platform_device_put(pdev); return ERR_PTR(r); }
    return pdev;
}

struct platform_device *platform_device_register_simple(const char *name, int id,
                                                        const struct resource *res, unsigned int num)
{
    return register_full(NULL, name, id, res, num, NULL, 0);
}

struct platform_device *platform_device_register_data(struct device *parent, const char *name, int id,
                                                      const void *data, size_t size)
{
    return register_full(parent, name, id, NULL, 0, data, size);
}

int __platform_driver_register(struct platform_driver *drv, struct module *owner)
{
    drv->driver.owner = owner;
    drv->driver.bus = &platform_bus_type;
    return driver_register(&drv->driver);
}

void platform_driver_unregister(struct platform_driver *drv) { driver_unregister(&drv->driver); }

struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num)
{
    for (u32 i = 0; i < dev->num_resources; i++) {
        struct resource *r = &dev->resource[i];
        if (resource_type(r) == type && num-- == 0) return r;
    }
    return NULL;
}

struct resource *platform_get_resource_byname(struct platform_device *dev, unsigned int type, const char *name)
{
    for (u32 i = 0; i < dev->num_resources; i++) {
        struct resource *r = &dev->resource[i];
        if (resource_type(r) == type && r->name && !strcmp(r->name, name)) return r;
    }
    return NULL;
}

int platform_get_irq_optional(struct platform_device *dev, unsigned int num)
{
    struct resource *r = platform_get_resource(dev, IORESOURCE_IRQ, num);
    return r ? (int)r->start : -ENXIO;
}

int platform_get_irq(struct platform_device *dev, unsigned int num)
{
    int r = platform_get_irq_optional(dev, num);
    if (r < 0) dev_err(&dev->dev, "IRQ index %u not found\n", num);
    return r;
}

void __iomem *devm_platform_ioremap_resource(struct platform_device *pdev, unsigned int index)
{
    return devm_ioremap_resource(&pdev->dev, platform_get_resource(pdev, IORESOURCE_MEM, index));
}

void lkpi_device_core_init(void)
{
    if (bus_register(&platform_bus_type)) lkpi_die("linuxkpi: cannot register the platform bus");
}
