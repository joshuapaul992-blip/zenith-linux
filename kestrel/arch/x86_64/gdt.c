/* arch/x86_64/gdt.c -- 64-bit GDT + TSS
 *
 *   0x00 null
 *   0x08 kernel code (DPL0, L)     0x10 kernel data (DPL0)
 *   0x18 user data   (DPL3)        0x20 user code   (DPL3, L)
 *   0x28 + 16 * n  TSS of CPU n (16-byte system descriptors)
 *
 * User data precedes user code because SYSRET loads SS = STAR[63:48]+8 and
 * CS = STAR[63:48]+16. The TSS supplies RSP0 (kernel stack on a ring 3 ->
 * ring 0 transition) and the Interrupt Stack Table: exceptions that can
 * arrive when the current stack cannot be trusted switch to a known-good
 * stack of their own,
 *   IST1  #DF  double fault (e.g. a kernel stack overflow hit its guard page)
 *   IST2  NMI  can interrupt anything, including the first instructions of
 *              another handler
 *   IST3  #MC  machine check: hardware error, state unknown */
#include <kernel/arch.h>
#include <kernel/smp.h>
#include <kernel/mm.h>
#include <kernel/string.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

/* One GDT for all CPUs; each CPU has its own TSS (and descriptor: a TSS
 * descriptor is marked busy by the CPU that loads it) and IST stacks. */
static uint64_t gdt[5 + 2 * MAX_CPUS];
static struct tss tss[MAX_CPUS];
#define IST_STACK_SIZE 16384
static uint8_t bsp_ist[3][IST_STACK_SIZE] __attribute__((aligned(16)));

struct gdt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

static void set_tss_desc(int cpu)
{
    uint64_t base = (uint64_t)&tss[cpu], limit = sizeof tss[cpu] - 1;
    gdt[5 + 2 * cpu] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
                       (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6 + 2 * cpu] = base >> 32;
}

/* Load the GDT, reload the segment registers, and this CPU's TSS. */
void gdt_load_cpu(int cpu)
{
    struct gdt_ptr p = { sizeof gdt - 1, (uint64_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"                       /* reload CS */
        "1:\n"
        "movw %w2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"             /* zeroes the GS base: percpu set after */
        "movw %w3, %%ax\n"
        "ltr %%ax\n"
        :: "m"(p), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "r"((uint16_t)(GDT_TSS + 16 * cpu))
        : "rax", "memory");
}

/* TSS of a CPU: its IST stacks (static for the boot CPU, allocated for the
 * others). Returns false if stacks could not be allocated. */
bool tss_setup_cpu(int cpu)
{
    memset(&tss[cpu], 0, sizeof tss[cpu]);
    for (int i = 0; i < 3; i++) {
        uint8_t *stk = cpu == 0 ? bsp_ist[i] : (uint8_t *)(uintptr_t)pmm_alloc_contig(IST_STACK_SIZE / PAGE_SIZE);
        if (!stk) return false;
        cpus[cpu].ist[i] = stk;
        tss[cpu].ist[i] = (uint64_t)(stk + IST_STACK_SIZE);
    }
    tss[cpu].iopb_offset = sizeof tss[cpu];
    cpus[cpu].tss = &tss[cpu];
    set_tss_desc(cpu);
    return true;
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull;     /* kernel code */
    gdt[2] = 0x00CF92000000FFFFull;     /* kernel data */
    gdt[3] = 0x00CFF2000000FFFFull;     /* user data   */
    gdt[4] = 0x00AFFA000000FFFFull;     /* user code   */
    tss_setup_cpu(0);
    gdt_load_cpu(0);
}

/* RSP0 of the running CPU (GS must point at its percpu block). */
void tss_set_kernel_stack(uint64_t rsp0) { ((struct tss *)this_cpu()->tss)->rsp[0] = rsp0; }

bool ist_stack_range(uint64_t addr, uint64_t *lo, uint64_t *hi)
{
    for (int c = 0; c < MAX_CPUS; c++)
        for (int i = 0; i < 3; i++) {
            uint64_t b = (uint64_t)cpus[c].ist[i];
            if (b && addr >= b && addr < b + IST_STACK_SIZE) { *lo = b; *hi = b + IST_STACK_SIZE; return true; }
        }
    return false;
}
