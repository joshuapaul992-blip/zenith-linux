/* asm/page.h -- Kestrel's Linux KPI: pages, and the identity map */
#ifndef _LKPI_ASM_PAGE_H
#define _LKPI_ASM_PAGE_H

#include <linux/const.h>
#include <linux/types.h>

#define PAGE_SHIFT              12
#define PAGE_SIZE               (_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK               (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr)        __ALIGN_KERNEL(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr)   ((addr) & PAGE_MASK)
#define PAGE_ALIGNED(addr)      (((unsigned long)(addr) & (PAGE_SIZE - 1)) == 0)
#define PAGE_OFFSET             0UL

/* virtual == physical */
#define __pa(x)                 ((phys_addr_t)(unsigned long)(x))
#define __va(x)                 ((void *)(unsigned long)(x))
#define __pa_symbol(x)          __pa(x)
static inline phys_addr_t virt_to_phys(volatile void *a) { return (phys_addr_t)(unsigned long)a; }
static inline void *phys_to_virt(phys_addr_t p) { return (void *)(unsigned long)p; }
#define virt_addr_valid(a)      ((unsigned long)(a) >= 4096)

#endif
