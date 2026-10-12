/* linux/ioport.h -- Kestrel's Linux KPI: resources
 *
 * request_region()/request_mem_region() always succeed and keep no
 * resource tree (a stub: Kestrel does not track who owns what). */
#ifndef _LKPI_LINUX_IOPORT_H
#define _LKPI_LINUX_IOPORT_H

#include <linux/types.h>
#include <linux/bits.h>

struct device;

struct resource {
    resource_size_t start;
    resource_size_t end;            /* inclusive */
    const char     *name;
    unsigned long   flags;
    unsigned long   desc;
    struct resource *parent, *sibling, *child;
};

#define IORESOURCE_BITS         0x000000ff
#define IORESOURCE_TYPE_BITS    0x00001f00
#define IORESOURCE_IO           0x00000100
#define IORESOURCE_MEM          0x00000200
#define IORESOURCE_REG          0x00000300
#define IORESOURCE_IRQ          0x00000400
#define IORESOURCE_DMA          0x00000800
#define IORESOURCE_BUS          0x00001000
#define IORESOURCE_PREFETCH     0x00002000
#define IORESOURCE_READONLY     0x00004000
#define IORESOURCE_CACHEABLE    0x00008000
#define IORESOURCE_RANGELENGTH  0x00010000
#define IORESOURCE_SHADOWABLE   0x00020000
#define IORESOURCE_SIZEALIGN    0x00040000
#define IORESOURCE_STARTALIGN   0x00080000
#define IORESOURCE_MEM_64       0x00100000
#define IORESOURCE_WINDOW       0x00200000
#define IORESOURCE_MUXED        0x00400000
#define IORESOURCE_EXCLUSIVE    0x08000000
#define IORESOURCE_DISABLED     0x10000000
#define IORESOURCE_UNSET        0x20000000
#define IORESOURCE_AUTO         0x40000000
#define IORESOURCE_BUSY         0x80000000
#define IORESOURCE_ROM_ENABLE   (1 << 0)
#define IORESOURCE_ROM_SHADOW   (1 << 1)

#define DEFINE_RES_NAMED(_start, _size, _name, _flags) \
    { .start = (_start), .end = (_start) + (_size) - 1, .name = (_name), .flags = (_flags), .desc = 0 }
#define DEFINE_RES_MEM(_start, _size)   DEFINE_RES_NAMED((_start), (_size), NULL, IORESOURCE_MEM)
#define DEFINE_RES_IO(_start, _size)    DEFINE_RES_NAMED((_start), (_size), NULL, IORESOURCE_IO)
#define DEFINE_RES_IRQ(_irq)            DEFINE_RES_NAMED((_irq), 1, NULL, IORESOURCE_IRQ)

extern struct resource ioport_resource;
extern struct resource iomem_resource;

static inline resource_size_t resource_size(const struct resource *r) { return r->end - r->start + 1; }
static inline unsigned long resource_type(const struct resource *r) { return r->flags & IORESOURCE_TYPE_BITS; }
static inline bool resource_contains(const struct resource *r1, const struct resource *r2)
{ return resource_type(r1) == resource_type(r2) && r1->start <= r2->start && r1->end >= r2->end; }

struct resource *__request_region(struct resource *parent, resource_size_t start, resource_size_t n,
                                  const char *name, int flags);
void __release_region(struct resource *parent, resource_size_t start, resource_size_t n);
#define request_region(start, n, name)          __request_region(&ioport_resource, (start), (n), (name), 0)
#define request_muxed_region(start, n, name)    __request_region(&ioport_resource, (start), (n), (name), IORESOURCE_MUXED)
#define request_mem_region(start, n, name)      __request_region(&iomem_resource, (start), (n), (name), 0)
#define request_mem_region_exclusive(s, n, nm)  __request_region(&iomem_resource, (s), (n), (nm), IORESOURCE_EXCLUSIVE)
#define release_region(start, n)                __release_region(&ioport_resource, (start), (n))
#define release_mem_region(start, n)            __release_region(&iomem_resource, (start), (n))
struct resource *__devm_request_region(struct device *dev, struct resource *parent, resource_size_t start,
                                       resource_size_t n, const char *name);
#define devm_request_region(dev, start, n, name)     __devm_request_region(dev, &ioport_resource, (start), (n), (name))
#define devm_request_mem_region(dev, start, n, name) __devm_request_region(dev, &iomem_resource, (start), (n), (name))
static inline int request_resource(struct resource *root, struct resource *n) { return 0; }
static inline int release_resource(struct resource *old) { return 0; }

#endif
