/* include/kernel/mm.h -- physical frames, kernel heap, page mapping */
#ifndef KESTREL_MM_H
#define KESTREL_MM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE      4096ul
#define PMM_MAX_PHYS   (4ul << 30)     /* frames tracked: first 4 GiB */

extern char _kernel_start[], _kernel_end[];

/* ---- physical memory manager (bitmap, 4 KiB frames) --------------------- */
void     pmm_init(uintptr_t mbi);
uint64_t pmm_alloc(void);                    /* one zeroed frame, 0 on OOM  */
uint64_t pmm_alloc_contig(size_t frames);    /* physically contiguous run   */
void     pmm_free(uint64_t phys);
void     pmm_free_contig(uint64_t phys, size_t frames);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);
uint64_t pmm_highest_usable(void);

/* ---- kernel heap ------------------------------------------------------- */
void   heap_init(size_t bytes);
void  *kmalloc(size_t n);
void  *kzalloc(size_t n);
void  *krealloc(void *p, size_t n);
void   kfree(void *p);
char  *kstrdup(const char *s);
size_t heap_used(void);
size_t heap_size(void);

/* ---- virtual memory ---------------------------------------------------- */
#define VMM_WRITE   0x002
#define VMM_USER    0x004
#define VMM_PWT     0x008
#define VMM_PCD     0x010
/* Identity-map [phys, phys+size) with 2 MiB pages (used for frame buffers
 * the firmware places above the 4 GiB boot mapping). */
bool vmm_identity_map(uint64_t phys, uint64_t size, uint64_t flags);
/* Make an identity-mapped MMIO range uncached (PCD|PWT), mapping it first if
 * needed. Granularity is the 2 MiB boot page, which for MMIO holes is fine. */
bool vmm_set_uncached(uint64_t phys, uint64_t size);
/* Map an identity-mapped frame buffer write-combining (PAT), mapping it first
 * if needed; 2 MiB pages already made uncached are left alone. False if the
 * CPU has no PAT. */
bool vmm_set_write_combining(uint64_t phys, uint64_t size);

#endif
