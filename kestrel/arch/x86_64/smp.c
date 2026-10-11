/* arch/x86_64/smp.c -- per-CPU blocks, the big kernel lock, TLB shootdowns,
 * and starting the other processors (see include/kernel/smp.h).
 *
 * Start-up of an application processor (AP), one at a time: the boot CPU
 * copies the trampoline (trampoline.asm) to 0x8000, gives it a stack and
 * the AP's percpu block, and sends INIT + STARTUP IPIs. The AP comes up in
 * real mode, switches to long mode on the kernel's page tables and calls
 * ap_entry(): GDT/TSS/IDT, GS base, SYSCALL MSRs, the boot CPU's CR0/CR4
 * and PAT, its local APIC and timer. It reports itself online and becomes
 * that CPU's idle thread. If an AP does not answer within 200 ms, no
 * further APs are started (its slot and stack stay reserved). kestrel.smp=0 (or maxcpus=1) keeps the system on one CPU. */
#include <kernel/smp.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/time.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/syscall.h>

struct percpu cpus[MAX_CPUS];
int ncpus = 1;
volatile bool smp_panicking;

#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_PAT            0x277
#define ADDR_MASK          0x000FFFFFFFFFF000ull
#define TRAMP_BASE         0x8000

void percpu_init_bsp(void)
{
    struct percpu *c = &cpus[0];
    c->self = c;
    c->id = 0;
    c->online = true;
    c->loaded_cr3 = read_cr3() & ADDR_MASK;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

bool percpu_is_kernel_gs(uint64_t base)
{
    uint64_t lo = (uint64_t)&cpus[0], hi = (uint64_t)&cpus[MAX_CPUS];
    return base >= lo && base < hi && (base - lo) % sizeof(struct percpu) == 0;
}

/* ---- the big kernel lock -------------------------------------------------------- */
static volatile uint32_t bkl_word;
static volatile int bkl_owner = -1;

void bkl_enter(void)
{
    uint64_t f = irq_save();                            /* no migration, no nesting mid-acquire */
    struct percpu *c = this_cpu();
    if (c->bkl_depth == 0) {
        while (__atomic_exchange_n(&bkl_word, 1, __ATOMIC_ACQUIRE)) {
            do { tlb_service(); cpu_relax(); }          /* the holder may wait for our TLB */
            while (__atomic_load_n(&bkl_word, __ATOMIC_RELAXED));
        }
        bkl_owner = c->id;
    }
    c->bkl_depth++;
    irq_restore(f);
}

void bkl_exit(void)
{
    uint64_t f = irq_save();
    struct percpu *c = this_cpu();
    if (c->bkl_depth <= 0) panic("bkl_exit: not held on CPU %d", c->id);
    if (--c->bkl_depth == 0) {
        bkl_owner = -1;
        __atomic_store_n(&bkl_word, 0, __ATOMIC_RELEASE);
    }
    irq_restore(f);
}

int bkl_depth(void) { return this_cpu()->bkl_depth; }
void bkl_set_depth(int d) { this_cpu()->bkl_depth = d; }

int bkl_release_all(void)
{
    uint64_t f = irq_save();
    int d = this_cpu()->bkl_depth;
    if (d) { this_cpu()->bkl_depth = 1; bkl_exit(); }
    irq_restore(f);
    return d;
}

void bkl_reacquire(int depth)
{
    if (!depth) return;
    uint64_t f = irq_save();
    bkl_enter();
    this_cpu()->bkl_depth = depth;
    irq_restore(f);
}

/* ---- TLB shootdowns ------------------------------------------------------------- */
#define TLB_KERNEL (~0ull)
#define FLUSH_PAGES_MAX 32

static void flush_local(uint64_t start, uint64_t end)
{
    if (end - start > FLUSH_PAGES_MAX * PAGE_SIZE) { write_cr3(read_cr3()); return; }
    for (uint64_t va = start & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) invlpg(va);
}

void tlb_service(void)
{
    struct percpu *c = this_cpu();
    if (!__atomic_load_n(&c->tlb_pending, __ATOMIC_ACQUIRE)) return;
    uint64_t p = c->tlb_pml4;
    if (p == TLB_KERNEL || (read_cr3() & ADDR_MASK) == p) flush_local(c->tlb_start, c->tlb_end);
    c->tlb_flushes++;
    __atomic_store_n(&c->tlb_pending, 0, __ATOMIC_RELEASE);
}

static void shootdown(uint64_t pml4, uint64_t start, uint64_t end)
{
    uint64_t f = irq_save();                            /* stay on this CPU throughout */
    struct percpu *me = this_cpu();
    if (pml4 == TLB_KERNEL || (read_cr3() & ADDR_MASK) == pml4) flush_local(start, end);
    if (ncpus == 1) { irq_restore(f); return; }
    bool sent[MAX_CPUS] = { false };
    for (int i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = &cpus[i];
        if (c == me || !c->online) continue;
        if (pml4 != TLB_KERNEL && c->loaded_cr3 != pml4) continue;   /* nothing cached there */
        while (__atomic_load_n(&c->tlb_pending, __ATOMIC_ACQUIRE)) { tlb_service(); cpu_relax(); }
        c->tlb_pml4 = pml4;
        c->tlb_start = start;
        c->tlb_end = end;
        __atomic_store_n(&c->tlb_pending, 1, __ATOMIC_RELEASE);
        lapic_send_ipi(c->apic_id, VEC_TLB);
        c->ipis++;
        sent[i] = true;
    }
    for (int i = 0; i < MAX_CPUS; i++)                  /* wait: the old frame may be freed next */
        if (sent[i]) while (__atomic_load_n(&cpus[i].tlb_pending, __ATOMIC_ACQUIRE)) { tlb_service(); cpu_relax(); }
    irq_restore(f);
}

void tlb_shootdown(uint64_t pml4, uint64_t start, uint64_t end) { shootdown(pml4 & ADDR_MASK, start, end); }
void tlb_shootdown_kernel(uint64_t start, uint64_t end) { shootdown(TLB_KERNEL, start, end); }

/* ---- scheduling help ----------------------------------------------------------- */
void smp_kick_idle(void)
{
    if (ncpus == 1) return;
    uint64_t f = irq_save();
    struct percpu *me = this_cpu();
    for (int i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = &cpus[i];
        if (c != me && c->online && __atomic_load_n(&c->in_idle, __ATOMIC_ACQUIRE)) {
            c->in_idle = false;                         /* one kick per idle period */
            lapic_send_ipi(c->apic_id, VEC_RESCHED);
            c->ipis++;
            break;
        }
    }
    irq_restore(f);
}

void smp_stop_others(void)
{
    if (ncpus == 1 || smp_panicking) { smp_panicking = true; return; }
    smp_panicking = true;
    lapic_send_nmi_all_others();                        /* NMI: even with interrupts off */
}

/* ---- application processors ------------------------------------------------------ */
extern uint8_t tramp_start[], tramp_end[], tramp_cr3[], tramp_efer[], tramp_stack[], tramp_entry[], tramp_arg[];
extern void syscall_cpu_init(void);
extern void idt_load(void);
extern struct tcb *sched_cpu_idle_task(int cpu, uint8_t *stack, size_t size);

static volatile bool ap_started;
static uint64_t bsp_cr0, bsp_cr4, bsp_pat;

static void tramp_put(uint8_t *field, uint64_t v)
{
    *(volatile uint64_t *)(uintptr_t)(TRAMP_BASE + (field - tramp_start)) = v;
}

__attribute__((noreturn)) static void ap_entry(struct percpu *c)
{
    gdt_load_cpu(c->id);
    idt_load();
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    __asm__ volatile("mov %0, %%cr4" :: "r"(bsp_cr4));  /* SSE (OSFXSR/OSXMMEXCPT), as the boot CPU */
    __asm__ volatile("mov %0, %%cr0" :: "r"(bsp_cr0));
    wrmsr(MSR_PAT, bsp_pat);                            /* write-combining entry */
    syscall_cpu_init();
    lapic_init_cpu();
    lapic_timer_start();
    c->loaded_cr3 = read_cr3() & ADDR_MASK;
    c->apic_id = lapic_id();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    __atomic_store_n(&ap_started, true, __ATOMIC_RELEASE);
    bkl_enter();                                        /* the idle loop is entered holding it */
    sched_idle_loop();                                  /* this CPU's idle thread */
}

void smp_boot_aps(void)
{
    cpus[0].apic_id = lapic_id();
    if (!apic_enabled()) return;
    if (strstr(g_boot.cmdline, "kestrel.smp=0") || strstr(g_boot.cmdline, "maxcpus=1")) {
        kprintf("smp: one CPU (kestrel.smp=0)\n");
        return;
    }
    if (apic_cpu_count() < 2) return;
    if (pmm_low_page() != TRAMP_BASE) {
        kprintf("smp: page %x is not usable RAM: one CPU\n", TRAMP_BASE);
        return;
    }
    if ((read_cr3() & ADDR_MASK) >= (4ull << 30)) { kprintf("smp: kernel page tables above 4 GiB\n"); return; }
    bsp_cr0 = read_cr0();
    bsp_cr4 = read_cr4();
    bsp_pat = rdmsr(MSR_PAT);
    memcpy((void *)(uintptr_t)TRAMP_BASE, tramp_start, (size_t)(tramp_end - tramp_start));
    tramp_put(tramp_cr3, read_cr3() & ADDR_MASK);
    tramp_put(tramp_efer, rdmsr(MSR_EFER) & ~(1ull << 10));        /* LMA is read-only */
    tramp_put(tramp_entry, (uint64_t)ap_entry);

    for (int i = 0; i < apic_cpu_count() && ncpus < MAX_CPUS; i++) {
        uint32_t id = apic_cpu_id(i);
        if (id == cpus[0].apic_id) continue;
        int n = ncpus;
        struct percpu *c = &cpus[n];
        memset(c, 0, sizeof *c);
        c->id = n;
        c->apic_id = id;
        uint8_t *stack = (uint8_t *)(uintptr_t)pmm_alloc_contig(KSTACK_SIZE / PAGE_SIZE);
        if (!stack || !tss_setup_cpu(n)) { kprintf("smp: out of memory for CPU %d\n", n); break; }
        c->idle = c->current = sched_cpu_idle_task(n, stack, KSTACK_SIZE);
        if (!c->idle) break;
        tramp_put(tramp_stack, ((uint64_t)stack + KSTACK_SIZE) & ~0xFull);
        tramp_put(tramp_arg, (uint64_t)c);
        ap_started = false;
        lapic_send_init_sipi(id, TRAMP_BASE >> 12);
        uint64_t t = time_ns();
        while (!__atomic_load_n(&ap_started, __ATOMIC_ACQUIRE) && time_ns() - t < 200000000ull) cpu_relax();
        if (!ap_started) {                              /* it may still come up late: */
            kprintf("smp: CPU with APIC ID %u did not start; no more CPUs\n", id);   /* reuse nothing */
            break;
        }
        ncpus++;
    }
    kprintf("smp: %d CPU(s) online\n", ncpus);
}
