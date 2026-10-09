/* mm/uvm.c -- per-process address spaces (see include/kernel/uvm.h) */
#include <kernel/uvm.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>

#define PTE_P       0x001ull
#define PTE_W       0x002ull
#define PTE_U       0x004ull
#define PTE_HUGE    0x080ull
#define ADDR_MASK   0x000FFFFFFFFFF000ull
#define KERNEL_SLOTS 128                    /* PML4[0..127] */

static uint64_t kernel_pml4;

uint64_t uvm_kernel_pml4(void) { return kernel_pml4; }

void uvm_init(void)
{
    kernel_pml4 = read_cr3() & ADDR_MASK;
    uint64_t *pml4 = (uint64_t *)kernel_pml4;
    int added = 0;
    for (int i = 0; i < KERNEL_SLOTS; i++) {
        if (pml4[i] & PTE_P) continue;
        uint64_t t = pmm_alloc();           /* zeroed, identity mapped */
        if (!t) panic("uvm: out of memory for kernel PDPTs");
        pml4[i] = t | PTE_P | PTE_W;
        added++;
    }
    write_cr3(read_cr3());

    /* SSE for user code (the kernel itself is built without it):
     * CR0.EM off, CR0.MP on, CR4.OSFXSR + OSXMMEXCPT on, x87 reset. */
    uint64_t cr0 = read_cr0(), cr4 = read_cr4();
    cr0 = (cr0 & ~0x4ull) | 0x2ull;
    cr4 |= (1ull << 9) | (1ull << 10);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");
    kprintf("uvm: kernel PML4 %lx, %d shared PDPTs pre-allocated; user space %lx-%lx\n",
            kernel_pml4, added, (uint64_t)UVM_USER_START, (uint64_t)(UVM_USER_END - 1));
}

uint64_t uvm_create(void)
{
    uint64_t p = pmm_alloc();
    if (!p) return 0;
    uint64_t *n = (uint64_t *)p, *k = (uint64_t *)kernel_pml4;
    for (int i = 0; i < KERNEL_SLOTS; i++) n[i] = k[i];
    return p;
}

bool uvm_range_ok(uint64_t va, uint64_t len)
{
    return va >= UVM_USER_START && len <= UVM_USER_END - UVM_USER_START && va + len <= UVM_USER_END;
}

/* Walk to the PTE for `va`, allocating user page tables on the way. */
static uint64_t *pte_of(uint64_t pml4, uint64_t va, bool alloc)
{
    uint64_t *t = (uint64_t *)pml4;
    static const int shift[3] = { 39, 30, 21 };
    for (int l = 0; l < 3; l++) {
        uint64_t *e = &t[(va >> shift[l]) & 511];
        if (!(*e & PTE_P)) {
            if (!alloc) return NULL;
            uint64_t n = pmm_alloc();
            if (!n) return NULL;
            *e = n | PTE_P | PTE_W | PTE_U;
        }
        if (*e & PTE_HUGE) return NULL;     /* never in the user half */
        t = (uint64_t *)(*e & ADDR_MASK);
    }
    return &t[(va >> 12) & 511];
}

bool uvm_map(uint64_t pml4, uint64_t va, uint64_t len, int prot)
{
    if ((va | len) & (UVM_PAGE - 1) || !uvm_range_ok(va, len)) return false;
    for (uint64_t a = va; a < va + len; a += UVM_PAGE) {
        uint64_t *pte = pte_of(pml4, a, true);
        if (!pte) return false;
        if (*pte & PTE_P) {                 /* keep contents, widen rights */
            if (prot & UVM_W) *pte |= PTE_W;
            continue;
        }
        uint64_t f = pmm_alloc();
        if (!f) return false;
        *pte = f | PTE_P | PTE_U | ((prot & UVM_W) ? PTE_W : 0);
    }
    if ((read_cr3() & ADDR_MASK) == pml4) write_cr3(read_cr3());
    return true;
}

void uvm_unmap(uint64_t pml4, uint64_t va, uint64_t len)
{
    if (!uvm_range_ok(va, len)) return;
    for (uint64_t a = va & ~(UVM_PAGE - 1); a < va + len; a += UVM_PAGE) {
        uint64_t *pte = pte_of(pml4, a, false);
        if (!pte || !(*pte & PTE_P)) continue;
        pmm_free(*pte & ADDR_MASK);
        *pte = 0;
    }
    if ((read_cr3() & ADDR_MASK) == pml4) write_cr3(read_cr3());
}

bool uvm_mapped(uint64_t pml4, uint64_t va, uint64_t len)
{
    if (!uvm_range_ok(va, len)) return false;
    for (uint64_t a = va & ~(UVM_PAGE - 1); a < va + len; a += UVM_PAGE) {
        uint64_t *pte = pte_of(pml4, a, false);
        if (!pte || !(*pte & PTE_P)) return false;
    }
    return true;
}

/* Visit the user half: free (or count) every page and page table. */
static uint64_t walk(uint64_t *t, int level, bool free_it)
{
    uint64_t n = 0;
    for (int i = (level == 4 ? KERNEL_SLOTS : 0); i < (level == 4 ? 256 : 512); i++) {
        if (!(t[i] & PTE_P)) continue;
        uint64_t phys = t[i] & ADDR_MASK;
        if (level == 1) n++;
        else n += walk((uint64_t *)phys, level - 1, free_it);
        if (free_it) { pmm_free(phys); t[i] = 0; }
    }
    return n;
}

uint64_t uvm_pages(uint64_t pml4) { return pml4 ? walk((uint64_t *)pml4, 4, false) : 0; }

void uvm_destroy(uint64_t pml4)
{
    if (!pml4 || pml4 == kernel_pml4) return;
    walk((uint64_t *)pml4, 4, true);
    pmm_free(pml4);
}
