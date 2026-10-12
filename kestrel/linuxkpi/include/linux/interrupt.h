/* linux/interrupt.h -- Kestrel's Linux KPI: interrupt handlers (linuxkpi/src/irq.c)
 *
 * Linux IRQ numbers here name MSI vectors the KPI allocated for a PCI
 * device (pci_alloc_irq_vectors, pci_irq_vector). The handler runs in
 * Kestrel's interrupt context (interrupts off); a threaded handler's
 * thread_fn runs in a kernel thread of its own. Tasklets run in a
 * kernel thread too (there are no softirqs). */
#ifndef _LKPI_LINUX_INTERRUPT_H
#define _LKPI_LINUX_INTERRUPT_H

#include <linux/types.h>
#include <linux/irqreturn.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/bitops.h>

struct device;
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

#define IRQF_TRIGGER_NONE       0x00000000
#define IRQF_TRIGGER_RISING     0x00000001
#define IRQF_TRIGGER_FALLING    0x00000002
#define IRQF_TRIGGER_HIGH       0x00000004
#define IRQF_TRIGGER_LOW        0x00000008
#define IRQF_SHARED             0x00000080
#define IRQF_PROBE_SHARED       0x00000100
#define IRQF_TIMER              0x00000200
#define IRQF_PERCPU             0x00000400
#define IRQF_NOBALANCING        0x00000800
#define IRQF_IRQPOLL            0x00001000
#define IRQF_ONESHOT            0x00002000
#define IRQF_NO_SUSPEND         0x00004000
#define IRQF_FORCE_RESUME       0x00008000
#define IRQF_NO_THREAD          0x00010000
#define IRQF_EARLY_RESUME       0x00020000
#define IRQF_COND_SUSPEND       0x00040000
#define IRQF_NO_AUTOEN          0x00080000
#define IRQF_NO_DEBUG           0x00100000

int  request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                          unsigned long flags, const char *name, void *dev_id);
static inline int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                              const char *name, void *dev_id)
{ return request_threaded_irq(irq, handler, NULL, flags, name, dev_id); }
static inline int request_any_context_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                                          const char *name, void *dev_id)
{ return request_threaded_irq(irq, handler, NULL, flags, name, dev_id); }
const void *free_irq(unsigned int irq, void *dev_id);
int  devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                               unsigned long flags, const char *name, void *dev_id);
static inline int devm_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
                                   unsigned long flags, const char *name, void *dev_id)
{ return devm_request_threaded_irq(dev, irq, handler, NULL, flags, name, dev_id); }
void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void enable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
static inline bool disable_hardirq(unsigned int irq) { disable_irq_nosync(irq); return true; }
static inline int irq_set_affinity_hint(unsigned int irq, const void *m) { return 0; }
static inline int irq_update_affinity_hint(unsigned int irq, const void *m) { return 0; }
static inline int enable_irq_wake(unsigned int irq) { return 0; }
static inline int disable_irq_wake(unsigned int irq) { return 0; }

/* ---- tasklets ---- */
struct tasklet_struct {
    struct list_head    entry;
    unsigned long       state;          /* bit 0: scheduled, bit 1: running */
    atomic_t            count;          /* > 0: disabled */
    bool                use_callback;
    union {
        void (*func)(unsigned long data);
        void (*callback)(struct tasklet_struct *t);
    };
    unsigned long       data;
};
#define TASKLET_STATE_SCHED     0
#define TASKLET_STATE_RUN       1
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);
void tasklet_setup(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *));
void tasklet_schedule(struct tasklet_struct *t);
#define tasklet_hi_schedule(t)  tasklet_schedule(t)
void tasklet_kill(struct tasklet_struct *t);
static inline void tasklet_disable_nosync(struct tasklet_struct *t) { atomic_inc(&t->count); }
void tasklet_disable(struct tasklet_struct *t);
static inline void tasklet_enable(struct tasklet_struct *t) { atomic_dec(&t->count); }
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) \
    container_of(callback_tasklet, typeof(*var), tasklet_fieldname)
#define DECLARE_TASKLET(name, _callback) \
    struct tasklet_struct name = { .entry = { NULL, NULL }, .count = ATOMIC_INIT(0), .use_callback = true, .callback = _callback }

#endif
