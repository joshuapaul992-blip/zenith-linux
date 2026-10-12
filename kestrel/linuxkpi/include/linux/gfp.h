/* linux/gfp.h -- Kestrel's Linux KPI: whole pages (linuxkpi/src/page.c) */
#ifndef _LKPI_LINUX_GFP_H
#define _LKPI_LINUX_GFP_H

#include <linux/gfp_types.h>
#include <linux/mm_types.h>
#include <asm/page.h>

#define MAX_PAGE_ORDER          10
#define MAX_ORDER               MAX_PAGE_ORDER
#include <linux/numa.h>

/* 2^order physically contiguous frames */
struct page *alloc_pages(gfp_t gfp, unsigned int order);
void __free_pages(struct page *page, unsigned int order);
#define alloc_page(gfp)                 alloc_pages(gfp, 0)
#define alloc_pages_node(nid, gfp, o)   alloc_pages(gfp, o)
#define alloc_pages_exact_nid(nid, size, gfp) alloc_pages_exact(size, gfp)
#define __free_page(page)               __free_pages((page), 0)
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
unsigned long get_zeroed_page(gfp_t gfp);
void free_pages(unsigned long addr, unsigned int order);
#define __get_free_page(gfp)            __get_free_pages((gfp), 0)
#define free_page(addr)                 free_pages((addr), 0)
void *alloc_pages_exact(size_t size, gfp_t gfp);
void free_pages_exact(void *virt, size_t size);

#endif
