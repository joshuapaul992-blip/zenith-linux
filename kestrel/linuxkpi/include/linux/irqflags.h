/* linux/irqflags.h -- Kestrel's Linux KPI: local interrupt control
 *
 * The KPI keeps interrupts off while anything holds a spinlock or has
 * preemption disabled (Kestrel preempts kernel threads from the timer
 * interrupt; with interrupts off nothing can). So enabling interrupts
 * inside such a section is deferred to its end (linuxkpi/src/sched.c). */
#ifndef _LKPI_LINUX_IRQFLAGS_H
#define _LKPI_LINUX_IRQFLAGS_H

#include <linux/typecheck.h>
#include <asm/irqflags.h>

void lkpi_local_irq_enable(void);
#define local_irq_enable()              lkpi_local_irq_enable()
#define local_irq_disable()             arch_local_irq_disable()
#define local_irq_save(flags)           do { typecheck(unsigned long, flags); flags = arch_local_irq_save(); } while (0)
#define local_irq_restore(flags)        do { typecheck(unsigned long, flags); \
                                             if (!arch_irqs_disabled_flags(flags)) lkpi_local_irq_enable(); \
                                             else arch_local_irq_disable(); } while (0)
#define local_save_flags(flags)         do { typecheck(unsigned long, flags); flags = arch_local_save_flags(); } while (0)
#define irqs_disabled()                 arch_irqs_disabled()
#define irqs_disabled_flags(flags)      arch_irqs_disabled_flags(flags)
#define raw_local_irq_save(flags)       local_irq_save(flags)
#define raw_local_irq_restore(flags)    local_irq_restore(flags)
#define lockdep_assert_irqs_disabled()  do { } while (0)
#define lockdep_assert_irqs_enabled()   do { } while (0)

#endif
