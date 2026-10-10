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
#include <kernel/uvm.h>

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_HUGE    0x080
#define ADDR_MASK   0x000FFFFFFFFFF000ull

/* Always edit the kernel's tables, whichever process' CR3 is loaded: the
 * PDPTs under PML4[0..127] are shared by every address space (uvm.c). */
static uint64_t *kpml4(void)
{
    uint64_t k = uvm_kernel_pml4();
    return (uint64_t *)(k ? k : (read_cr3() & ADDR_MASK));
}

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
    uint64_t *pml4 = kpml4();
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

/* Write-combining for frame buffers. Firmware MTRRs normally make video
 * memory uncached, which turns every frame-buffer write into a single slow
 * bus transaction. A write-combining PAT type overrides an uncached MTRR, so
 * PAT entry 1 (PWT=1, PCD=0 -- unused otherwise) is reprogrammed from
 * write-through to write-combining, as Linux does, and frame-buffer pages
 * are mapped with PWT alone. */
#define MSR_PAT     0x277
#define PAT_WC      0x01

static bool pat_wc_ready(void)
{
    static int state;                               /* 0 untried, 1 ok, -1 no PAT */
    if (state) return state > 0;
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 16))) { state = -1; return false; }
    uint64_t f = irq_save();
    uint64_t pat = (rdmsr(MSR_PAT) & ~(0xFFull << 8)) | ((uint64_t)PAT_WC << 8);
    __asm__ volatile("wbinvd" ::: "memory");
    wrmsr(MSR_PAT, pat);
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());                          /* drop TLB entries with the old type */
    irq_restore(f);
    state = 1;
    return true;
}

bool vmm_set_write_combining(uint64_t phys, uint64_t size)
{
    if (!size || !pat_wc_ready() || !vmm_identity_map(phys, size, VMM_WRITE)) return false;
    uint64_t *pml4 = kpml4();
    int pages = 0;
    for (uint64_t a = phys & ~0x1FFFFFull; a < phys + size; a += 0x200000) {
        uint64_t *pdpt = (uint64_t *)(pml4[(a >> 39) & 511] & ADDR_MASK);
        uint64_t *pd = (uint64_t *)(pdpt[(a >> 30) & 511] & ADDR_MASK);
        uint64_t *pde = &pd[(a >> 21) & 511];
        if (!(*pde & PTE_HUGE) || (*pde & VMM_PCD)) continue;   /* MMIO made uncached stays so */
        *pde = (*pde & ~(uint64_t)VMM_PCD) | VMM_PWT;
        invlpg(a);
        pages++;
    }
    __asm__ volatile("wbinvd" ::: "memory");
    return pages > 0;
}

/* The 4 KiB PTE of identity-mapped `phys`, splitting its 2 MiB page into a
 * page table first if needed (same physical pages, same attributes). The
 * boot identity map lives in the shared kernel PDPTs, so the change is seen
 * by every address space. NULL if unmapped or out of memory. */
#define PTE_NX      (1ull << 63)
#define PDE_PAT     (1ull << 12)                        /* PAT bit of a 2 MiB PDE */
static uint64_t *identity_pte(uint64_t phys)
{
    uint64_t *pml4 = kpml4();
    if (!(pml4[(phys >> 39) & 511] & PTE_PRESENT)) return NULL;
    uint64_t *pdpt = (uint64_t *)(pml4[(phys >> 39) & 511] & ADDR_MASK);
    if (!(pdpt[(phys >> 30) & 511] & PTE_PRESENT) || (pdpt[(phys >> 30) & 511] & PTE_HUGE)) return NULL;
    uint64_t *pd = (uint64_t *)(pdpt[(phys >> 30) & 511] & ADDR_MASK);
    uint64_t *pde = &pd[(phys >> 21) & 511];
    if (!(*pde & PTE_PRESENT)) return NULL;
    if (*pde & PTE_HUGE) {
        uint64_t pt_phys = pmm_alloc();                 /* zeroed, identity mapped */
        if (!pt_phys) return NULL;
        uint64_t *pt = (uint64_t *)pt_phys;
        uint64_t base = *pde & 0x000FFFFFFFE00000ull;
        uint64_t attr = *pde & (PTE_PRESENT | PTE_WRITE | VMM_PWT | VMM_PCD | PTE_NX);
        if (*pde & PDE_PAT) attr |= 0x80;               /* PAT bit moves to bit 7 in a PTE */
        for (int i = 0; i < 512; i++) pt[i] = (base + (uint64_t)i * 4096) | attr;
        *pde = pt_phys | PTE_PRESENT | PTE_WRITE;
        /* invlpg on any address of a large page drops its TLB entry */
        invlpg(base);
        write_cr3(read_cr3());
    }
    uint64_t *pt = (uint64_t *)(*pde & ADDR_MASK);
    return &pt[(phys >> 12) & 511];
}

bool vmm_set_guard(uint64_t phys, bool guard)
{
    uint64_t f = irq_save();
    uint64_t *pte = identity_pte(phys & ~0xFFFull);
    if (pte) {
        if (guard) *pte &= ~(uint64_t)PTE_PRESENT; else *pte |= PTE_PRESENT;
        invlpg(phys & ~0xFFFull);
    }
    irq_restore(f);
    return pte != NULL;
}

bool vmm_identity_map(uint64_t phys, uint64_t size, uint64_t flags)
{
    uint64_t *pml4 = kpml4();
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
