/* linux/preempt.h -- Kestrel's Linux KPI: preemption and context (linuxkpi/src/sched.c)
 *
 * A per-CPU count, as in Linux: while it is non-zero the CPU runs with
 * interrupts off, so the thread can neither be preempted nor migrate. */
#ifndef _LKPI_LINUX_PREEMPT_H
#define _LKPI_LINUX_PREEMPT_H

#include <linux/types.h>
#include <linux/irqflags.h>

void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
int  lkpi_preempt_count(void);
int  lkpi_in_hardirq(void);

#define preempt_disable()               lkpi_preempt_disable()
#define preempt_enable()                lkpi_preempt_enable()
#define preempt_enable_no_resched()     lkpi_preempt_enable()
#define preempt_disable_notrace()       lkpi_preempt_disable()
#define preempt_enable_notrace()        lkpi_preempt_enable()
#define preempt_count()                 lkpi_preempt_count()
#define preemptible()                   (lkpi_preempt_count() == 0 && !irqs_disabled())
#define in_hardirq()                    lkpi_in_hardirq()
#define in_irq()                        lkpi_in_hardirq()
#define in_softirq()                    0
#define in_serving_softirq()            0
#define in_nmi()                        0
#define in_interrupt()                  lkpi_in_hardirq()
#define in_task()                       (!lkpi_in_hardirq())
#define in_atomic()                     (lkpi_preempt_count() != 0 || lkpi_in_hardirq())
#define local_bh_disable()              lkpi_preempt_disable()
#define local_bh_enable()               lkpi_preempt_enable()
#define migrate_disable()               lkpi_preempt_disable()
#define migrate_enable()                lkpi_preempt_enable()

#endif
