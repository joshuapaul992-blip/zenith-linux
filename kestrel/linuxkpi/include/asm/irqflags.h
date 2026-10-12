/* asm/irqflags.h -- Kestrel's Linux KPI: the x86 interrupt flag */
#ifndef _LKPI_ASM_IRQFLAGS_H
#define _LKPI_ASM_IRQFLAGS_H

#define X86_EFLAGS_IF   0x200UL

static inline unsigned long arch_local_save_flags(void)
{
    unsigned long f;
    __asm__ __volatile__("pushfq; popq %0" : "=rm"(f) :: "memory");
    return f;
}
static inline void arch_local_irq_disable(void) { __asm__ __volatile__("cli" ::: "memory"); }
static inline void arch_local_irq_enable(void) { __asm__ __volatile__("sti" ::: "memory"); }
static inline unsigned long arch_local_irq_save(void)
{
    unsigned long f = arch_local_save_flags();
    arch_local_irq_disable();
    return f;
}
static inline int arch_irqs_disabled_flags(unsigned long f) { return !(f & X86_EFLAGS_IF); }
static inline int arch_irqs_disabled(void) { return arch_irqs_disabled_flags(arch_local_save_flags()); }

#endif
