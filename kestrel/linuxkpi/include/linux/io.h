/* linux/io.h -- Kestrel's Linux KPI: device memory (linuxkpi/src/io.c) */
#ifndef _LKPI_LINUX_IO_H
#define _LKPI_LINUX_IO_H

#include <linux/types.h>
#include <linux/err.h>
#include <asm/io.h>

struct device;
struct resource;

/* Make a physical range usable as device memory: it is identity mapped,
 * so the address is the physical one, now uncached (or write-combining). */
void __iomem *ioremap(phys_addr_t phys, size_t size);
void __iomem *ioremap_wc(phys_addr_t phys, size_t size);
#define ioremap_uc(p, s)                ioremap(p, s)
#define ioremap_wt(p, s)                ioremap(p, s)
#define ioremap_np(p, s)                ioremap(p, s)
static inline void __iomem *ioremap_cache(phys_addr_t phys, size_t size) { return (void __iomem *)(unsigned long)phys; }
static inline void iounmap(volatile void __iomem *addr) { }
void *memremap(resource_size_t offset, size_t size, unsigned long flags);
static inline void memunmap(void *addr) { }
#define MEMREMAP_WB     1
#define MEMREMAP_WT     2
#define MEMREMAP_WC     4
void __iomem *ioport_map(unsigned long port, unsigned int nr);
static inline void ioport_unmap(void __iomem *addr) { }

void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size);
void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size);
void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res);
#define devm_ioremap_uc(d, o, s)        devm_ioremap(d, o, s)
static inline void devm_iounmap(struct device *dev, void __iomem *addr) { }

/* MTRRs and PAT: write-combining is set up by ioremap_wc already */
static inline int arch_phys_wc_add(unsigned long base, unsigned long size) { return 0; }
static inline void arch_phys_wc_del(int handle) { }
static inline int arch_phys_wc_index(int handle) { return -1; }
static inline int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size) { return 0; }
static inline void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size) { }
#define devm_arch_phys_wc_add(d, b, s)  0
#define devm_arch_io_reserve_memtype_wc(d, s, z) 0

#endif
