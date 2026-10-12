/* linuxkpi/src/page.c -- struct page and whole-page allocations
 *
 * Kestrel has no struct page, so the KPI keeps its own: a table indexed
 * by frame number, filled in chunks of 4096 pages as frames are first
 * looked at (memory is identity mapped, so a page's address is its
 * frame number shifted). alloc_pages() takes contiguous frames from
 * Kestrel's allocator; the head page keeps the reference count and the
 * order. */
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/printk.h>
#include "internal.h"

#define CHUNK_SHIFT     12                          /* 4096 pages: 16 MiB of memory */
#define CHUNK_PAGES     (1UL << CHUNK_SHIFT)
#define MAX_CHUNKS      4096                        /* up to 64 GiB of physical addresses */

static struct page *chunks[MAX_CHUNKS];
static DEFINE_SPINLOCK(chunk_lock);

struct page *pfn_to_page(unsigned long pfn)
{
    unsigned long c = pfn >> CHUNK_SHIFT;
    if (c >= MAX_CHUNKS) {
        printk(KERN_ERR "linuxkpi: no struct page for frame %#lx\n", pfn);
        return NULL;
    }
    struct page *chunk = READ_ONCE(chunks[c]);
    if (!chunk) {
        struct page *n = lkpi_heap_alloc(CHUNK_PAGES * sizeof(struct page));
        if (!n) return NULL;
        memset(n, 0, CHUNK_PAGES * sizeof(struct page));
        for (unsigned long i = 0; i < CHUNK_PAGES; i++) {
            n[i].lkpi_pfn = (c << CHUNK_SHIFT) + i;
            INIT_LIST_HEAD(&n[i].lru);
        }
        unsigned long flags;
        spin_lock_irqsave(&chunk_lock, flags);
        chunk = chunks[c];
        if (!chunk) chunks[c] = chunk = n;
        spin_unlock_irqrestore(&chunk_lock, flags);
        if (chunk != n) lkpi_heap_free(n);     /* someone else was first */
    }
    return &chunk[pfn & (CHUNK_PAGES - 1)];
}

struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
    if (order > MAX_PAGE_ORDER) return NULL;
    unsigned long phys = lkpi_frames_alloc(1UL << order, !!(gfp & (__GFP_DMA | __GFP_DMA32)));
    if (!phys) {
        if (!(gfp & __GFP_NOWARN))
            printk(KERN_WARNING "linuxkpi: page allocation failure: order %u\n", order);
        return NULL;
    }
    /* Kestrel's frames come zeroed, so __GFP_ZERO needs nothing more */
    struct page *page = pfn_to_page(phys >> PAGE_SHIFT);
    if (!page) { lkpi_frames_free(phys, 1UL << order); return NULL; }
    atomic_set(&page->_refcount, 1);
    page->lkpi_order = order;
    page->private = 0;
    page->mapping = NULL;
    return page;
}

static void free_frames(struct page *page, unsigned int order)
{
    lkpi_frames_free(page_to_pfn(page) << PAGE_SHIFT, 1UL << order);
}

void __free_pages(struct page *page, unsigned int order)
{
    if (page && atomic_dec_and_test(&page->_refcount)) free_frames(page, order);
}

void put_page(struct page *page)
{
    if (atomic_dec_and_test(&page->_refcount)) free_frames(page, page->lkpi_order);
}

unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
    struct page *page = alloc_pages(gfp, order);
    return page ? (unsigned long)page_address(page) : 0;
}

unsigned long get_zeroed_page(gfp_t gfp) { return __get_free_pages(gfp | __GFP_ZERO, 0); }

void free_pages(unsigned long addr, unsigned int order)
{
    if (addr) __free_pages(virt_to_page((void *)addr), order);
}

/* exactly enough pages, not a power of two */
void *alloc_pages_exact(size_t size, gfp_t gfp)
{
    unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
    unsigned long phys = lkpi_frames_alloc(n, !!(gfp & (__GFP_DMA | __GFP_DMA32)));
    return phys ? (void *)phys : NULL;
}

void free_pages_exact(void *virt, size_t size)
{
    if (virt) lkpi_frames_free((unsigned long)virt, PAGE_ALIGN(size) >> PAGE_SHIFT);
}

unsigned long lkpi_totalram_pages(void) { return lkpi_ram_total() >> PAGE_SHIFT; }
