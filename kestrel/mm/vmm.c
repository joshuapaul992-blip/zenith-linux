/* mm/vmm.c -- page-table editing on top of the boot identity map.
 *
 * boot.asm identity-maps 0-4 GiB with 2 MiB pages, which covers the kernel,
 * all memory the PMM hands out, and the frame buffer on virtually every
 * BIOS/VBE system. Some UEFI firmware places the GOP frame buffer above
 * 4 GiB; vmm_identity_map() extends the mapping on demand. This is also the
 * hook where per-process address spaces will be built later. */
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_HUGE    0x080
#define ADDR_MASK   0x000FFFFFFFFFF000ull

static uint64_t *table_for(uint64_t *parent, int idx)
{
    if (!(parent[idx] & PTE_PRESENT)) {
        uint64_t t = pmm_alloc();               /* zeroed, identity mapped */
        if (!t) return NULL;
        parent[idx] = t | PTE_PRESENT | PTE_WRITE;
    }
    return (uint64_t *)(parent[idx] & ADDR_MASK);
}

bool vmm_set_uncached(uint64_t phys, uint64_t size)
{
    if (!vmm_identity_map(phys, size, VMM_WRITE | VMM_PCD | VMM_PWT)) return false;
    uint64_t *pml4 = (uint64_t *)(read_cr3() & ADDR_MASK);
    for (uint64_t a = phys & ~0x1FFFFFull; a < phys + size; a += 0x200000) {
        uint64_t *pdpt = (uint64_t *)(pml4[(a >> 39) & 511] & ADDR_MASK);
        uint64_t *pd = (uint64_t *)(pdpt[(a >> 30) & 511] & ADDR_MASK);
        uint64_t *pde = &pd[(a >> 21) & 511];
        if ((*pde & PTE_HUGE) && (*pde & (VMM_PCD | VMM_PWT)) != (VMM_PCD | VMM_PWT)) {
            *pde |= VMM_PCD | VMM_PWT;
            invlpg(a);
        }
    }
    return true;
}

bool vmm_identity_map(uint64_t phys, uint64_t size, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t *)(read_cr3() & ADDR_MASK);
    uint64_t start = phys & ~0x1FFFFFull;
    uint64_t end = (phys + size + 0x1FFFFF) & ~0x1FFFFFull;

    for (uint64_t a = start; a < end; a += 0x200000) {
        uint64_t *pdpt = table_for(pml4, (a >> 39) & 511);
        if (!pdpt) return false;
        uint64_t *pd = table_for(pdpt, (a >> 30) & 511);
        if (!pd) return false;
        uint64_t *pde = &pd[(a >> 21) & 511];
        if (!(*pde & PTE_PRESENT)) {
            *pde = a | PTE_PRESENT | PTE_HUGE | (flags & (VMM_WRITE | VMM_PWT | VMM_PCD | VMM_USER));
            invlpg(a);
        }
    }
    return true;
}
