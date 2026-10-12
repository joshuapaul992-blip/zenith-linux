/* linux/dmapool.h -- Kestrel's Linux KPI: pools of small coherent DMA blocks
 * (each block is its own allocation here: simple, and wasteful for tiny blocks) */
#ifndef _LKPI_LINUX_DMAPOOL_H
#define _LKPI_LINUX_DMAPOOL_H
#include <linux/types.h>
#include <linux/gfp_types.h>
struct device;
struct dma_pool;
struct dma_pool *dma_pool_create(const char *name, struct device *dev, size_t size, size_t align, size_t boundary);
void dma_pool_destroy(struct dma_pool *pool);
void *dma_pool_alloc(struct dma_pool *pool, gfp_t flags, dma_addr_t *handle);
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr);
static inline void *dma_pool_zalloc(struct dma_pool *pool, gfp_t flags, dma_addr_t *handle)
{ return dma_pool_alloc(pool, flags | __GFP_ZERO, handle); }
struct dma_pool *dmam_pool_create(const char *name, struct device *dev, size_t size, size_t align, size_t allocation);
#endif
