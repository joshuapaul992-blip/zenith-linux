/* linux/dma-mapping.h -- Kestrel's Linux KPI: DMA (linuxkpi/src/dma.c)
 *
 * No IOMMU and an identity map: a DMA address is the physical address,
 * and x86 keeps caches coherent with devices, so mapping is a
 * translation and syncing is a compiler barrier. Coherent buffers are
 * whole frames, below 4 GiB when the device's mask says so. */
#ifndef _LKPI_LINUX_DMA_MAPPING_H
#define _LKPI_LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/dma-direction.h>
#include <linux/mm_types.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <asm/page.h>

struct device;

#define DMA_BIT_MASK(n)         (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR       (~(dma_addr_t)0)
#define DMA_ATTR_WEAK_ORDERING  (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE  (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC  (1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_ATTR_NO_WARN        (1UL << 8)

int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{ int rc = dma_set_mask(dev, mask); return rc ? rc : dma_set_coherent_mask(dev, mask); }
static inline int dma_coerce_mask_and_coherent(struct device *dev, u64 mask) { return dma_set_mask_and_coherent(dev, mask); }
u64 dma_get_mask(struct device *dev);
static inline u64 dma_get_required_mask(struct device *dev) { return DMA_BIT_MASK(64); }
static inline bool dma_addressing_limited(struct device *dev) { return false; }
static inline size_t dma_max_mapping_size(struct device *dev) { return ~(size_t)0; }
static inline unsigned int dma_get_max_seg_size(struct device *dev) { return 0xffffffffu; }
static inline int dma_set_max_seg_size(struct device *dev, unsigned int size) { return 0; }
static inline int dma_set_seg_boundary(struct device *dev, unsigned long mask) { return 0; }

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs);
void dma_free_attrs(struct device *dev, size_t size, void *vaddr, dma_addr_t handle, unsigned long attrs);
static inline void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp)
{ return dma_alloc_attrs(dev, size, handle, gfp, 0); }
static inline void dma_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t handle)
{ dma_free_attrs(dev, size, vaddr, handle, 0); }
#define dma_alloc_wc(d, s, h, g)        dma_alloc_attrs(d, s, h, g, DMA_ATTR_WRITE_COMBINE)
#define dma_free_wc(d, s, v, h)         dma_free_attrs(d, s, v, h, DMA_ATTR_WRITE_COMBINE)
#define dma_zalloc_coherent(d, s, h, g) dma_alloc_coherent(d, s, h, (g) | __GFP_ZERO)
void *dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs);
static inline void *dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp)
{ return dmam_alloc_attrs(dev, size, handle, gfp, 0); }
void dmam_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t handle);

/* streaming mappings: the physical address, or DMA_MAPPING_ERROR when the
 * buffer lies beyond the device's mask (there are no bounce buffers) */
dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir, unsigned long attrs);
#define dma_map_single(d, p, s, dir)            dma_map_single_attrs(d, p, s, dir, 0)
static inline void dma_unmap_single_attrs(struct device *dev, dma_addr_t a, size_t size,
                                          enum dma_data_direction dir, unsigned long attrs) { }
#define dma_unmap_single(d, a, s, dir)          dma_unmap_single_attrs(d, a, s, dir, 0)
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size,
                              enum dma_data_direction dir, unsigned long attrs);
#define dma_map_page(d, p, o, s, r)             dma_map_page_attrs(d, p, o, s, r, 0)
static inline void dma_unmap_page_attrs(struct device *dev, dma_addr_t a, size_t size,
                                        enum dma_data_direction dir, unsigned long attrs) { }
#define dma_unmap_page(d, a, s, r)              dma_unmap_page_attrs(d, a, s, r, 0)
static inline dma_addr_t dma_map_resource(struct device *dev, phys_addr_t phys, size_t size,
                                          enum dma_data_direction dir, unsigned long attrs) { return phys; }
static inline void dma_unmap_resource(struct device *dev, dma_addr_t a, size_t size,
                                      enum dma_data_direction dir, unsigned long attrs) { }
static inline int dma_mapping_error(struct device *dev, dma_addr_t a) { return a == DMA_MAPPING_ERROR ? -ENOMEM : 0; }
#define __lkpi_dma_sync(d, a, s, dir)   __asm__ __volatile__("" ::: "memory")
#define dma_sync_single_for_cpu(d, a, s, dir)           __lkpi_dma_sync(d, a, s, dir)
#define dma_sync_single_for_device(d, a, s, dir)        __lkpi_dma_sync(d, a, s, dir)
#define dma_sync_single_range_for_cpu(d, a, o, s, dir)  __lkpi_dma_sync(d, a, s, dir)
#define dma_sync_single_range_for_device(d, a, o, s, dir) __lkpi_dma_sync(d, a, s, dir)
#define dma_sync_sg_for_cpu(d, sg, n, dir)              __lkpi_dma_sync(d, sg, n, dir)
#define dma_sync_sg_for_device(d, sg, n, dir)           __lkpi_dma_sync(d, sg, n, dir)
#define dma_sync_sgtable_for_cpu(d, st, dir)            __lkpi_dma_sync(d, st, 0, dir)
#define dma_sync_sgtable_for_device(d, st, dir)         __lkpi_dma_sync(d, st, 0, dir)
int  dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir,
                      unsigned long attrs);
#define dma_map_sg(d, s, n, r)          dma_map_sg_attrs(d, s, n, r, 0)
static inline void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
                                      enum dma_data_direction dir, unsigned long attrs) { }
#define dma_unmap_sg(d, s, n, r)        dma_unmap_sg_attrs(d, s, n, r, 0)
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                                  unsigned long attrs)
{ int n = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs); if (n <= 0) return -EINVAL; sgt->nents = n; return 0; }
static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir,
                                     unsigned long attrs) { }
#define dma_unmap_addr(PTR, ADDR_NAME)          ((PTR)->ADDR_NAME)
#define dma_unmap_addr_set(PTR, ADDR_NAME, VAL) (((PTR)->ADDR_NAME) = (VAL))
#define dma_unmap_len(PTR, LEN_NAME)            ((PTR)->LEN_NAME)
#define dma_unmap_len_set(PTR, LEN_NAME, VAL)   (((PTR)->LEN_NAME) = (VAL))
#define DEFINE_DMA_UNMAP_ADDR(ADDR_NAME)        dma_addr_t ADDR_NAME
#define DEFINE_DMA_UNMAP_LEN(LEN_NAME)          __u32 LEN_NAME

#endif
