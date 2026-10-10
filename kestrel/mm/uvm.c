/* mm/uvm.c -- per-process address spaces (see include/kernel/uvm.h) */
#include <kernel/uvm.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>

#define PTE_P       0x001ull
#define PTE_W       0x002ull
#define PTE_U       0x004ull
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
    cr0 |= 1ull << 18;                      /* AM: user code that sets EFLAGS.AC gets #AC (SIGBUS) */
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
