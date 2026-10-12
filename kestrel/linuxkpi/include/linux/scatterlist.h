/* linux/scatterlist.h -- Kestrel's Linux KPI: scatter-gather lists (linuxkpi/src/dma.c)
 * A table is one flat array (no chaining); entries hold the page directly. */
#ifndef _LKPI_LINUX_SCATTERLIST_H
#define _LKPI_LINUX_SCATTERLIST_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/gfp_types.h>
#include <asm/page.h>

struct scatterlist {
    struct page    *lkpi_page;
    unsigned int    offset;
    unsigned int    length;
    dma_addr_t      dma_address;
    unsigned int    dma_length;
    bool            lkpi_last;
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int        nents;          /* mapped */
    unsigned int        orig_nents;
};

#define sg_dma_address(sg)      ((sg)->dma_address)
#define sg_dma_len(sg)          ((sg)->dma_length)
static inline struct page *sg_page(struct scatterlist *sg) { return sg->lkpi_page; }
static inline bool sg_is_last(struct scatterlist *sg) { return sg->lkpi_last; }
static inline struct scatterlist *sg_next(struct scatterlist *sg) { return sg->lkpi_last ? NULL : sg + 1; }
static inline void sg_mark_end(struct scatterlist *sg) { sg->lkpi_last = true; }
static inline void sg_unmark_end(struct scatterlist *sg) { sg->lkpi_last = false; }
static inline void sg_assign_page(struct scatterlist *sg, struct page *page) { sg->lkpi_page = page; }
static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset)
{ sg->lkpi_page = page; sg->offset = offset; sg->length = len; }
void sg_init_table(struct scatterlist *sgl, unsigned int nents);
void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen);
void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen);
static inline dma_addr_t sg_phys(struct scatterlist *sg) { return ((dma_addr_t)sg->lkpi_page->lkpi_pfn << PAGE_SHIFT) + sg->offset; }
static inline void *sg_virt(struct scatterlist *sg) { return (void *)(unsigned long)sg_phys(sg); }
int  sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp);
void sg_free_table(struct sg_table *table);
int  sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
                               unsigned int offset, unsigned long size, gfp_t gfp);
int  sg_nents(struct scatterlist *sg);
#define for_each_sg(sglist, sg, nr, __i) \
    for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))
#define for_each_sgtable_sg(sgt, sg, i)         for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i)     for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)

#endif
