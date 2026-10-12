/* linuxkpi/src/io.c -- ioremap, port mappings and I/O resources
 *
 * Kestrel identity maps memory, so ioremap() returns the physical address
 * after making that range uncached (or write-combining) in the kernel map.
 * Port I/O "mappings" are cookies below PIO_RESERVED that ioread*() and
 * iowrite*() turn back into in/out instructions (asm/io.h).
 *
 * Resource requests (request_region and friends) always succeed and are
 * only remembered so they can be released: Kestrel keeps no tree of who
 * owns which addresses. */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include "internal.h"

struct resource ioport_resource = { .start = 0, .end = 0xffff, .name = "PCI IO", .flags = IORESOURCE_IO };
struct resource iomem_resource = { .start = 0, .end = ~(resource_size_t)0, .name = "PCI mem", .flags = IORESOURCE_MEM };

static void __iomem *map(phys_addr_t phys, size_t size, int wc)
{
    if (!size) return NULL;
    if (lkpi_map_mmio((unsigned long)phys, size, wc)) {
        printk(KERN_ERR "linuxkpi: cannot map MMIO %#llx (+%#zx)\n", (unsigned long long)phys, size);
        return NULL;
    }
    return (void __iomem *)(unsigned long)phys;
}

void __iomem *ioremap(phys_addr_t phys, size_t size) { return map(phys, size, 0); }
void __iomem *ioremap_wc(phys_addr_t phys, size_t size) { return map(phys, size, 1); }

void *memremap(resource_size_t offset, size_t size, unsigned long flags)
{
    if (flags & MEMREMAP_WB) return (void *)(unsigned long)offset;      /* ordinary memory */
    return (void __force *)map(offset, size, (flags & MEMREMAP_WC) ? 1 : 0);
}

void __iomem *ioport_map(unsigned long port, unsigned int nr)
{
    if (port > PIO_MASK) return NULL;
    return (void __iomem *)(PIO_OFFSET + port);
}

/* ---- managed mappings ------------------------------------------------------------- */
void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size)
{
    return ioremap(offset, size);                       /* nothing to undo */
}

void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size)
{
    return ioremap_wc(offset, size);
}

void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res)
{
    if (!res || resource_type(res) != IORESOURCE_MEM) {
        dev_err(dev, "invalid resource %pR\n", res);
        return (void __iomem *)ERR_PTR(-EINVAL);
    }
    if (!__devm_request_region(dev, &iomem_resource, res->start, resource_size(res), dev_name(dev)))
        return (void __iomem *)ERR_PTR(-EBUSY);
    void __iomem *p = ioremap(res->start, resource_size(res));
    return p ? p : (void __iomem *)ERR_PTR(-ENOMEM);
}

/* ---- resources ------------------------------------------------------------------------- */
static DEFINE_SPINLOCK(res_lock);
static LIST_HEAD(requests);

struct request { struct list_head node; struct resource res; };

struct resource *__request_region(struct resource *parent, resource_size_t start, resource_size_t n,
                                  const char *name, int flags)
{
    struct request *r = kzalloc(sizeof *r, GFP_KERNEL);
    unsigned long f;
    if (!r) return NULL;
    r->res.start = start;
    r->res.end = start + n - 1;
    r->res.name = name;
    r->res.flags = parent->flags | IORESOURCE_BUSY | flags;
    r->res.parent = parent;
    spin_lock_irqsave(&res_lock, f);
    list_add(&r->node, &requests);
    spin_unlock_irqrestore(&res_lock, f);
    return &r->res;
}

void __release_region(struct resource *parent, resource_size_t start, resource_size_t n)
{
    struct request *r, *found = NULL;
    unsigned long f;
    spin_lock_irqsave(&res_lock, f);
    list_for_each_entry(r, &requests, node) {
        if (r->res.parent == parent && r->res.start == start && r->res.end == start + n - 1) {
            list_del(&r->node);
            found = r;
            break;
        }
    }
    spin_unlock_irqrestore(&res_lock, f);
    if (found) kfree(found);
    else printk(KERN_WARNING "linuxkpi: trying to free nonexistent resource <%#llx-%#llx>\n",
                (unsigned long long)start, (unsigned long long)(start + n - 1));
}

struct region_devres { struct resource *parent; resource_size_t start, n; };

static void devm_region_release(struct device *dev, void *res)
{
    struct region_devres *r = res;
    __release_region(r->parent, r->start, r->n);
}

struct resource *__devm_request_region(struct device *dev, struct resource *parent, resource_size_t start,
                                       resource_size_t n, const char *name)
{
    struct region_devres *dr = devres_alloc(devm_region_release, sizeof *dr, GFP_KERNEL);
    if (!dr) return NULL;
    struct resource *res = __request_region(parent, start, n, name, 0);
    if (!res) { devres_free(dr); return NULL; }
    dr->parent = parent;
    dr->start = start;
    dr->n = n;
    devres_add(dev, dr);
    return res;
}
