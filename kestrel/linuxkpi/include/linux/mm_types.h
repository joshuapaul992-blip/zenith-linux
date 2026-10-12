/* linux/mm_types.h -- Kestrel's Linux KPI: struct page
 *
 * Kestrel keeps no per-frame structure; the KPI makes one on demand for
 * every frame Linux code allocates or asks about (linuxkpi/src/page.c),
 * from a table indexed by frame number. Memory is identity mapped, so a
 * page's address is its physical address. */
#ifndef _LKPI_LINUX_MM_TYPES_H
#define _LKPI_LINUX_MM_TYPES_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>

struct address_space;

struct page {
    unsigned long           flags;
    unsigned long           lkpi_pfn;
    struct list_head        lru;
    struct address_space   *mapping;
    unsigned long           index;
    unsigned long           private;
    atomic_t                _refcount;
    atomic_t                _mapcount;
    unsigned int            lkpi_order;     /* of the allocation it heads */
};

struct folio { struct page page; };
struct mm_struct { int dummy; };
struct vm_area_struct;
typedef unsigned long vm_flags_t;

#endif
