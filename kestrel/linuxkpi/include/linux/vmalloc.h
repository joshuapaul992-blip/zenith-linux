/* linux/vmalloc.h -- Kestrel's Linux KPI: "virtually contiguous" memory
 *
 * Memory is identity mapped, so vmalloc is a page-aligned kmalloc
 * (physically contiguous too). vmap() of scattered pages has no mapping
 * to build: it works only for pages that happen to be contiguous. */
#ifndef _LKPI_LINUX_VMALLOC_H
#define _LKPI_LINUX_VMALLOC_H

#include <linux/types.h>
#include <linux/gfp_types.h>
#include <linux/mm_types.h>

void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void  vfree(const void *addr);
#define vmalloc_node(s, node)           vmalloc(s)
#define vzalloc_node(s, node)           vzalloc(s)
#define vmalloc_user(s)                 vzalloc(s)
#define vmalloc_32(s)                   vmalloc(s)
#define __vmalloc(s, gfp)               ((gfp) & __GFP_ZERO ? vzalloc(s) : vmalloc(s))
void *vmalloc_array(size_t n, size_t size);
void *vcalloc(size_t n, size_t size);

#define VM_MAP                          0x00000004
#define VM_IOREMAP                      0x00000001
void *vmap(struct page **pages, unsigned int count, unsigned long flags, unsigned long prot);
void  vunmap(const void *addr);
#define PAGE_KERNEL                     0
#define PAGE_KERNEL_IO                  0
#define pgprot_writecombine(p)          (p)
#define pgprot_noncached(p)             (p)

#endif
