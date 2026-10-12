/* include/kernel/smp.h -- processors: per-CPU data, local/IO APIC, IPIs,
 * TLB shootdowns and the big kernel lock (arch/x86_64/apic.c, smp.c)
 *
 * Per-CPU data. Each processor has a struct percpu reached through the GS
 * segment base while it runs kernel code: entry from ring 3 (interrupt,
 * exception, SYSCALL) executes SWAPGS, which exchanges the user's GS base
 * with IA32_KERNEL_GS_BASE; the return to ring 3 swaps back. NMI, #MC and
 * #DF can arrive at any instruction -- also between SYSCALL and its SWAPGS
 * -- so their entry reads the GS base and only swaps when it is not one of
 * the percpu blocks ("paranoid" entry).
 *
 * Interrupts. With an ACPI MADT the legacy 8259 PICs are masked: ISA
 * interrupts (the PS/2 keyboard) come through the IO APIC to the boot CPU,
 * and every CPU has its own local APIC timer (1 kHz, calibrated against
 * the HPET/TSC clock) for its time slices. The boot CPU's timer also
 * drives the system tick. Without an MADT (or with kestrel.noapic) the
 * kernel stays on the PIC and the PIT, one CPU.
 *
 * The big kernel lock. Kernel code was written for one CPU: its critical
 * sections are "interrupts off". On several CPUs a single recursive lock
 * keeps that true: a CPU holds it whenever it executes kernel code
 * (taken on entry from ring 3 or from the idle loop, released on the way
 * back), so kernel code runs on one CPU at a time while user code runs on
 * all of them. The lock depth is saved per task across context switches.
 * Waiting for the lock services TLB shootdown requests, so a CPU holding
 * it can always complete a shootdown. Futex buckets, epoll and the other
 * spinlocks stay correct without it and are the starting point for
 * finer-grained locking.
 *
 * TLB shootdowns. Changing or removing a user mapping invalidates the TLB
 * of every CPU that has that address space loaded (CR3) -- invlpg for a
 * few pages, a CR3 reload otherwise -- and waits for each to acknowledge
 * before the page is freed or reused. Kernel identity-map changes (guard
 * pages, cache types) go to all CPUs. */
#ifndef KESTREL_SMP_H
#define KESTREL_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/kstat.h>

#define MAX_CPUS        16

/* Interrupt vectors above the ISA range */
#define VEC_RESCHED     0xE0            /* IPI: look at the run queue      */
#define VEC_TLB         0xE1            /* IPI: TLB shootdown request      */
#define VEC_STOP        0xE2            /* IPI: halt (panic on another CPU)*/
#define VEC_LAPIC_TIMER 0xEF
#define VEC_SPURIOUS    0xFF

struct tcb;

struct percpu {
    struct percpu *self;                /* gs:0                                */
    uint64_t       kernel_rsp;          /* gs:8   SYSCALL: top of kernel stack */
    uint64_t       user_rsp;            /* gs:16  SYSCALL: user RSP scratch    */
    struct tcb    *current;             /* gs:24  running task                 */
    int            id;                  /* 0 = boot CPU                        */
    uint32_t       apic_id;
    bool           online;
    struct tcb    *idle;                /* this CPU's idle thread              */
    bool           need_resched;
    bool           in_idle;             /* halted in the idle loop             */
    int            bkl_depth;           /* big kernel lock nesting             */
    uint64_t       loaded_cr3;          /* address space in CR3                */
    /* TLB shootdown mailbox: written by the requester, cleared when done */
    volatile uint64_t tlb_pml4;         /* 0: none pending; ~0: kernel (all)   */
    volatile uint64_t tlb_start, tlb_end;
    volatile int   tlb_pending;
    uint64_t       ticks;               /* local timer interrupts              */
    uint64_t       tlb_flushes, ipis;
    void          *tss;                 /* arch/x86_64/gdt.c                   */
    uint8_t       *ist[3];              /* #DF, NMI, #MC stacks                */
    struct kstat_cpu ks;                /* performance counters (kstat.h)      */
};

_Static_assert(__builtin_offsetof(struct percpu, kernel_rsp) == 8, "gs:8");
_Static_assert(__builtin_offsetof(struct percpu, user_rsp) == 16, "gs:16");
_Static_assert(__builtin_offsetof(struct percpu, current) == 24, "gs:24");

extern struct percpu cpus[MAX_CPUS];
extern int ncpus;                       /* CPUs online                         */

static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

/* The running task, in one GS-relative load: atomic with respect to
 * preemption. (this_cpu()->current is two loads: a thread preempted and
 * moved to another CPU in between would read the old CPU's task.) Any other
 * percpu field is only meaningful with interrupts off. */
static inline struct tcb *this_task(void)
{
    struct tcb *t;
    __asm__ volatile("mov %%gs:24, %0" : "=r"(t));
    return t;
}

void percpu_init_bsp(void);             /* right after gdt_init                */
bool percpu_is_kernel_gs(uint64_t base);

/* ---- APIC (arch/x86_64/apic.c) ------------------------------------------ */
bool     apic_init(void);               /* MADT, LAPIC, IO APIC; false: stay on the PIC */
bool     apic_enabled(void);
void     lapic_init_cpu(void);          /* this CPU's LAPIC (and timer, once calibrated) */
void     lapic_timer_start(void);       /* calibrate (boot CPU) and start, 1 kHz */
void     lapic_eoi(void);
uint32_t lapic_id(void);
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_nmi_all_others(void);
int      apic_cpu_count(void);          /* processors listed in the MADT       */
uint32_t apic_cpu_id(int i);            /* their APIC IDs                      */
void     lapic_send_init_sipi(uint32_t apic_id, uint8_t page);
bool     lapic_icr_wait(void);
void     ioapic_route_isa(int irq, uint8_t vector);    /* to the boot CPU      */

/* ---- SMP (arch/x86_64/smp.c) --------------------------------------------- */
void smp_boot_aps(void);                /* start the other processors          */
void smp_stop_others(void);             /* panic: halt every other CPU         */
extern volatile bool smp_panicking;

/* Big kernel lock */
void bkl_enter(void);                   /* nest (take it if this CPU has not)  */
void bkl_exit(void);                    /* unnest (release at depth 0)         */
bool bkl_try_enter(void);               /* nest, or take it only if it is free */
void bkl_exit_switch(void);             /* schedule(): release, deferred work left */
/* Interrupt work that needs the big lock does not wait for it: an
 * interrupt that finds it busy posts its work here, and the holder runs
 * it as it lets go (bkl_exit), interrupts off. Bits 0-31: MSI vector
 * VEC_MSI_BASE + bit (msi_dispatch); BKL_DEFER_TICK: the boot CPU's tick
 * hooks and interval timers (timer_tick_locked). */
#define BKL_DEFER_TICK (1ull << 32)
void bkl_defer(uint64_t work);          /* post, then run it now if the lock is free */
void bkl_run_deferred(void);            /* left by bkl_exit_switch: run it if free */
int  bkl_depth(void);
void bkl_set_depth(int d);              /* context switch: the next task's     */
int  bkl_release_all(void);             /* pure computation: returns the depth */
void bkl_reacquire(int depth);

/* TLB shootdowns */
void tlb_shootdown(uint64_t pml4, uint64_t start, uint64_t end);   /* a user address space */
void tlb_shootdown_kernel(uint64_t start, uint64_t end);          /* the shared kernel map */
void tlb_service(void);                 /* run a pending request for this CPU  */

/* Switch this CPU's address space (and record it for shootdowns). */
static inline void cpu_load_cr3(uint64_t cr3)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    this_cpu()->loaded_cr3 = cr3 & 0x000FFFFFFFFFF000ull;
}

/* Scheduler: make some CPU look at the run queue soon */
void smp_kick_idle(void);

#endif
