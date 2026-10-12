/* linux/mm.h -- Kestrel's Linux KPI: pages and addresses */
#ifndef _LKPI_LINUX_MM_H
#define _LKPI_LINUX_MM_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <asm/page.h>

struct page *pfn_to_page(unsigned long pfn);
static inline unsigned long page_to_pfn(const struct page *p) { return p->lkpi_pfn; }
#define page_to_phys(page)      ((phys_addr_t)page_to_pfn(page) << PAGE_SHIFT)
#define phys_to_page(phys)      pfn_to_page((phys) >> PAGE_SHIFT)
static inline void *page_address(const struct page *p) { return (void *)(p->lkpi_pfn << PAGE_SHIFT); }
#define lowmem_page_address(p)  page_address(p)
#define virt_to_page(addr)      pfn_to_page(__pa(addr) >> PAGE_SHIFT)
#define page_to_virt(p)         page_address(p)
#define virt_to_pfn(addr)       (__pa(addr) >> PAGE_SHIFT)
#define pfn_valid(pfn)          ((pfn) != 0)
#define offset_in_page(p)       ((unsigned long)(p) & ~PAGE_MASK)
#define page_folio(p)           ((struct folio *)(p))
#define folio_page(f, n)        (&(f)->page + (n))

static inline void get_page(struct page *p) { atomic_inc(&p->_refcount); }
void put_page(struct page *p);
static inline int page_count(const struct page *p) { return atomic_read(&p->_refcount); }
static inline void set_page_private(struct page *p, unsigned long v) { p->private = v; }
static inline unsigned long page_private(const struct page *p) { return p->private; }
static inline void set_page_dirty(struct page *p) { }
static inline int set_page_dirty_lock(struct page *p) { return 0; }
#define SetPageReserved(p)      do { } while (0)
#define ClearPageReserved(p)    do { } while (0)
#define PageHighMem(p)          0
#define is_vmalloc_addr(a)      lkpi_is_vmalloc_addr(a)
bool lkpi_is_vmalloc_addr(const void *a);
struct page *vmalloc_to_page(const void *addr);
unsigned long vmalloc_to_pfn(const void *addr);

unsigned long lkpi_totalram_pages(void);
#define totalram_pages()        lkpi_totalram_pages()

#endif
