/* linuxkpi/src/dma.c -- DMA mappings, coherent memory, pools and scatterlists
 *
 * There is no IOMMU and memory is identity mapped, so a DMA address is
 * the physical address is the virtual address. Coherent buffers are
 * whole frames (zeroed), below 4 GiB unless the device can reach more.
 * Streaming mappings are a translation and a check against the device's
 * mask: there are no bounce buffers, so memory the device cannot reach
 * gives DMA_MAPPING_ERROR. */
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/scatterlist.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include "internal.h"

int dma_set_mask(struct device *dev, u64 mask)
{
    if (!dev->dma_mask || mask < DMA_BIT_MASK(24)) return -EIO;
    *dev->dma_mask = mask;
    return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
    if (mask < DMA_BIT_MASK(24)) return -EIO;
    dev->coherent_dma_mask = mask;
    return 0;
}

u64 dma_get_mask(struct device *dev)
{
    return dev && dev->dma_mask && *dev->dma_mask ? *dev->dma_mask : DMA_BIT_MASK(32);
}

static dma_addr_t check(struct device *dev, phys_addr_t phys, size_t size)
{
    u64 mask = dma_get_mask(dev);
    if (size && phys + size - 1 > mask) {
        if (dev) dev_err(dev, "DMA to %#llx (%zu bytes) is beyond the mask %#llx; no bounce buffers\n",
                         (unsigned long long)phys, size, (unsigned long long)mask);
        return DMA_MAPPING_ERROR;
    }
    return phys;
}

dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir, unsigned long attrs)
{
    barrier();
    return check(dev, virt_to_phys(ptr), size);
}

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset, size_t size,
                              enum dma_data_direction dir, unsigned long attrs)
{
    barrier();
    return check(dev, page_to_phys(page) + offset, size);
}

int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents, enum dma_data_direction dir,
                     unsigned long attrs)
{
    struct scatterlist *s;
    int i;
    barrier();
    for_each_sg(sg, s, nents, i) {
        s->dma_address = check(dev, sg_phys(s), s->length);
        if (s->dma_address == DMA_MAPPING_ERROR) return 0;
        s->dma_length = s->length;
    }
    return nents;
}

/* ---- coherent memory ---------------------------------------------------------------- */
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
    u64 mask = dev ? dev->coherent_dma_mask : DMA_BIT_MASK(32);
    unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
    if (!n) return NULL;
    unsigned long phys = lkpi_frames_alloc(n, mask <= DMA_BIT_MASK(32) || (gfp & (__GFP_DMA | __GFP_DMA32)));
    if (!phys) return NULL;
    if (phys + size - 1 > mask) {                       /* a mask under 32 bits: not supported */
        lkpi_frames_free(phys, n);
        return NULL;
    }
    if (attrs & DMA_ATTR_WRITE_COMBINE) lkpi_map_mmio(phys, n << PAGE_SHIFT, 1);
    *handle = phys;
    return (void *)phys;                                /* frames come zeroed */
}

void dma_free_attrs(struct device *dev, size_t size, void *vaddr, dma_addr_t handle, unsigned long attrs)
{
    if (vaddr) lkpi_frames_free(handle, PAGE_ALIGN(size) >> PAGE_SHIFT);
}

struct dmam_res { void *vaddr; dma_addr_t handle; size_t size; unsigned long attrs; };

static void dmam_release(struct device *dev, void *res)
{
    struct dmam_res *r = res;
    dma_free_attrs(dev, r->size, r->vaddr, r->handle, r->attrs);
}

static int dmam_match(struct device *dev, void *res, void *data)
{
    return ((struct dmam_res *)res)->vaddr == data;
}

void *dmam_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
    struct dmam_res *r = devres_alloc(dmam_release, sizeof *r, gfp);
    if (!r) return NULL;
    void *v = dma_alloc_attrs(dev, size, handle, gfp, attrs);
    if (!v) { devres_free(r); return NULL; }
    r->vaddr = v;
    r->handle = *handle;
    r->size = size;
    r->attrs = attrs;
    devres_add(dev, r);
    return v;
}

void dmam_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t handle)
{
    devres_release(dev, dmam_release, dmam_match, vaddr);
}

/* ---- pools: small coherent blocks carved from pages ---------------------------------- */
struct dma_pool {
    char                name[32];
    struct device      *dev;
    size_t              size, align, boundary, chunk;
    struct mutex        lock;
    struct list_head    chunks;
    void               *free;               /* free blocks, linked through their first word */
};

struct pool_chunk { struct list_head node; void *vaddr; dma_addr_t dma; };

struct dma_pool *dma_pool_create(const char *name, struct device *dev, size_t size, size_t align, size_t boundary)
{
    if (!size) return NULL;
    if (!align) align = 8;
    if (align & (align - 1)) return NULL;
    size = ALIGN(size < sizeof(void *) ? sizeof(void *) : size, align);
    if (boundary && (boundary < size || (boundary & (boundary - 1)))) return NULL;
    struct dma_pool *p = kzalloc(sizeof *p, GFP_KERNEL);
    if (!p) return NULL;
    strscpy(p->name, name, sizeof p->name);
    p->dev = dev;
    p->size = size;
    p->align = align;
    p->boundary = boundary;
    p->chunk = size > PAGE_SIZE ? PAGE_ALIGN(size) : PAGE_SIZE;
    mutex_init(&p->lock);
    INIT_LIST_HEAD(&p->chunks);
    return p;
}

static int pool_grow(struct dma_pool *p, gfp_t gfp)
{
    struct pool_chunk *c = kzalloc(sizeof *c, gfp);
    if (!c) return -ENOMEM;
    c->vaddr = dma_alloc_coherent(p->dev, p->chunk, &c->dma, gfp);
    if (!c->vaddr) { kfree(c); return -ENOMEM; }
    list_add(&c->node, &p->chunks);
    for (size_t off = 0; off + p->size <= p->chunk; off += p->size) {
        if (p->boundary && off / p->boundary != (off + p->size - 1) / p->boundary)
            off = ALIGN(off, p->boundary);                  /* never across a boundary */
        if (off + p->size > p->chunk) break;
        void *b = (u8 *)c->vaddr + off;
        *(void **)b = p->free;
        p->free = b;
    }
    return 0;
}

void *dma_pool_alloc(struct dma_pool *pool, gfp_t flags, dma_addr_t *handle)
{
    mutex_lock(&pool->lock);
    if (!pool->free && pool_grow(pool, flags)) { mutex_unlock(&pool->lock); return NULL; }
    void *b = pool->free;
    pool->free = *(void **)b;
    mutex_unlock(&pool->lock);
    if (flags & __GFP_ZERO) memset(b, 0, pool->size);
    else *(void **)b = NULL;
    *handle = virt_to_phys(b);
    return b;
}

void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr)
{
    if (!vaddr) return;
    mutex_lock(&pool->lock);
    *(void **)vaddr = pool->free;
    pool->free = vaddr;
    mutex_unlock(&pool->lock);
}

void dma_pool_destroy(struct dma_pool *pool)
{
    if (!pool) return;
    struct pool_chunk *c, *n;
    list_for_each_entry_safe(c, n, &pool->chunks, node) {
        dma_free_coherent(pool->dev, pool->chunk, c->vaddr, c->dma);
        kfree(c);
    }
    kfree(pool);
}

static void dmam_pool_release(struct device *dev, void *res) { dma_pool_destroy(*(struct dma_pool **)res); }

struct dma_pool *dmam_pool_create(const char *name, struct device *dev, size_t size, size_t align, size_t allocation)
{
    struct dma_pool **r = devres_alloc(dmam_pool_release, sizeof *r, GFP_KERNEL);
    if (!r) return NULL;
    *r = dma_pool_create(name, dev, size, align, allocation);
    if (!*r) { devres_free(r); return NULL; }
    devres_add(dev, r);
    return *r;
}

/* ---- scatterlists: flat tables ----------------------------------------------------------- */
void sg_init_table(struct scatterlist *sgl, unsigned int nents)
{
    memset(sgl, 0, sizeof *sgl * nents);
    if (nents) sg_mark_end(&sgl[nents - 1]);
}

void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
    sg_set_page(sg, virt_to_page(buf), buflen, (unsigned int)offset_in_page(buf));
}

void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen)
{
    sg_init_table(sg, 1);
    sg_set_buf(sg, buf, buflen);
}

int sg_nents(struct scatterlist *sg)
{
    int n = 0;
    for (; sg; sg = sg_next(sg)) n++;
    return n;
}

int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp)
{
    memset(table, 0, sizeof *table);
    if (!nents) return -EINVAL;
    table->sgl = kmalloc_array(nents, sizeof *table->sgl, gfp);
    if (!table->sgl) return -ENOMEM;
    sg_init_table(table->sgl, nents);
    table->orig_nents = table->nents = nents;
    return 0;
}

void sg_free_table(struct sg_table *table)
{
    kfree(table->sgl);
    table->sgl = NULL;
    table->nents = table->orig_nents = 0;
}

/* one entry per run of contiguous pages */
int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
                              unsigned int offset, unsigned long size, gfp_t gfp)
{
    unsigned int runs = 0;
    for (unsigned int i = 0; i < n_pages; i++)
        if (!i || page_to_pfn(pages[i]) != page_to_pfn(pages[i - 1]) + 1) runs++;
    int r = sg_alloc_table(sgt, runs, gfp);
    if (r) return r;
    struct scatterlist *s = sgt->sgl;
    unsigned int i = 0;
    while (i < n_pages) {
        unsigned int j = i + 1;
        while (j < n_pages && page_to_pfn(pages[j]) == page_to_pfn(pages[j - 1]) + 1) j++;
        unsigned long len = (unsigned long)(j - i) * PAGE_SIZE - offset;
        if (len > size) len = size;
        sg_set_page(s, pages[i], (unsigned int)len, offset);
        size -= len;
        offset = 0;
        s = sg_next(s);
        i = j;
    }
    return 0;
}
