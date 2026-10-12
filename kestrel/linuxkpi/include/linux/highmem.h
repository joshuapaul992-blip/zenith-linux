/* linux/highmem.h -- Kestrel's Linux KPI: all memory is mapped, kmap is page_address */
#ifndef _LKPI_LINUX_HIGHMEM_H
#define _LKPI_LINUX_HIGHMEM_H
#include <linux/mm.h>
#include <linux/preempt.h>
#include <linux/string.h>
static inline void *kmap(struct page *p) { return page_address(p); }
static inline void kunmap(struct page *p) { }
static inline void *kmap_atomic(struct page *p) { preempt_disable(); return page_address(p); }
#define kunmap_atomic(addr)             do { (void)(addr); preempt_enable(); } while (0)
static inline void *kmap_local_page(struct page *p) { return page_address(p); }
#define kmap_local_folio(f, off)        ((char *)page_address(&(f)->page) + (off))
#define kunmap_local(addr)              do { (void)(addr); } while (0)
static inline void clear_highpage(struct page *p) { memset(page_address(p), 0, PAGE_SIZE); }
static inline void copy_highpage(struct page *to, struct page *from) { memcpy(page_address(to), page_address(from), PAGE_SIZE); }
static inline void memcpy_from_page(char *to, struct page *p, size_t off, size_t len) { memcpy(to, (char *)page_address(p) + off, len); }
static inline void memcpy_to_page(struct page *p, size_t off, const char *from, size_t len) { memcpy((char *)page_address(p) + off, from, len); }
static inline void memzero_page(struct page *p, size_t off, size_t len) { memset((char *)page_address(p) + off, 0, len); }
#define flush_dcache_page(p)            do { } while (0)
#endif
