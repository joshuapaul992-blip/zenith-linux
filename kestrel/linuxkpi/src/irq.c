/* linuxkpi/src/irq.c -- interrupt handlers, MSI vectors and tasklets
 *
 * Linux IRQ numbers here are the KPI's own (1 .. NR_IRQS-1). A PCI device
 * gets one when it is enabled; its Kestrel MSI vector is set up when the
 * driver asks for vectors (pci_alloc_irq_vectors), or when it requests
 * that IRQ without doing so. Kestrel calls trampoline() with interrupts
 * off; it runs the primary handlers and, for IRQ_WAKE_THREAD, wakes the
 * action's thread, which runs thread_fn.
 *
 * Kestrel routes MSI-X entry 0 or MSI, one vector per device, and no
 * legacy INTx (it has no _PRT routing): a device without MSI cannot have
 * an interrupt handler yet. */
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/string.h>
#include "internal.h"

#define NR_IRQS         64

struct irqaction {
    irq_handler_t       handler, thread_fn;
    void               *dev_id;
    unsigned int        irq;
    unsigned long       flags;
    char                name[32];
    struct task_struct *thread;
    unsigned long       thread_flags;           /* bit 0: run thread_fn */
    struct irqaction   *next;
};

struct irq_desc {
    bool                used;
    struct pci_dev     *pdev;
    int                 vector;                 /* Kestrel's, or -1 while lazy */
    bool                auto_enabled;           /* request_irq turned MSI on   */
    int                 depth;                  /* disable_irq nesting */
    atomic_t            in_progress;
    unsigned long       count;
    raw_spinlock_t      lock;
    struct irqaction   *actions;
};

static struct irq_desc descs[NR_IRQS];
static DEFINE_SPINLOCK(desc_lock);

static struct irq_desc *irq_to_desc(unsigned int irq)
{
    if (!irq || irq >= NR_IRQS || !descs[irq].used) return NULL;
    return &descs[irq];
}

static unsigned int desc_alloc(struct pci_dev *pdev)
{
    unsigned long flags;
    spin_lock_irqsave(&desc_lock, flags);
    for (unsigned int i = 1; i < NR_IRQS; i++) {
        if (!descs[i].used) {
            memset(&descs[i], 0, sizeof descs[i]);
            descs[i].used = true;
            descs[i].pdev = pdev;
            descs[i].vector = -1;
            raw_spin_lock_init(&descs[i].lock);
            spin_unlock_irqrestore(&desc_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&desc_lock, flags);
    return 0;
}

/* ---- the interrupt itself ---------------------------------------------------- */
static void trampoline(void *arg)
{
    struct irq_desc *desc = arg;
    unsigned int irq = (unsigned int)(desc - descs);

    lkpi_hardirq_enter();
    atomic_inc(&desc->in_progress);
    desc->count++;
    if (!desc->depth) {
        raw_spin_lock(&desc->lock);
        for (struct irqaction *a = desc->actions; a; a = a->next) {
            irqreturn_t r = a->handler ? a->handler((int)irq, a->dev_id) : IRQ_WAKE_THREAD;
            if ((r & IRQ_WAKE_THREAD) && a->thread) {
                set_bit(0, &a->thread_flags);
                wake_up_process(a->thread);
            }
        }
        raw_spin_unlock(&desc->lock);
    }
    atomic_dec(&desc->in_progress);
    lkpi_hardirq_exit();
}

static int irq_thread(void *arg)
{
    struct irqaction *a = arg;
    for (;;) {
        set_current_state(TASK_INTERRUPTIBLE);
        if (kthread_should_stop()) break;
        if (!test_and_clear_bit(0, &a->thread_flags)) {
            schedule();
            continue;
        }
        __set_current_state(TASK_RUNNING);
        a->thread_fn((int)a->irq, a->dev_id);
    }
    __set_current_state(TASK_RUNNING);
    return 0;
}

/* ---- PCI vectors --------------------------------------------------------------- */
static int msi_on(struct pci_dev *pdev, struct irq_desc *desc, unsigned int flags)
{
    if (pdev->no_msi || !(pdev->msi_cap || pdev->msix_cap)) return -ENOTSUPP;
    if (!(flags & (PCI_IRQ_MSI | PCI_IRQ_MSIX))) return -ENOTSUPP;
    if (!(flags & PCI_IRQ_MSIX) && !pdev->msi_cap) return -ENOTSUPP;
    if (!(flags & PCI_IRQ_MSI) && !pdev->msix_cap) return -ENOTSUPP;
    int v = lkpi_pci_msi_enable(pdev->lkpi_handle, trampoline, desc, pci_name(pdev));
    if (v < 0) return -ENOSPC;
    desc->vector = v;
    pdev->lkpi_vector = v;
    if (lkpi_pci_msi_is_msix(v)) pdev->msix_enabled = 1;
    else pdev->msi_enabled = 1;
    return 0;
}

static void msi_off(struct pci_dev *pdev, struct irq_desc *desc)
{
    if (desc->vector < 0) return;
    lkpi_pci_msi_disable(pdev->lkpi_handle, desc->vector);
    desc->vector = -1;
    pdev->lkpi_vector = -1;
    pdev->msi_enabled = pdev->msix_enabled = 0;
}

/* pci_enable_device: give the device its Linux IRQ number (no vector yet) */
void lkpi_pci_irq_setup(struct pci_dev *pdev)
{
    if (pdev->irq || !(pdev->pin || pdev->msi_cap || pdev->msix_cap)) return;
    pdev->irq = desc_alloc(pdev);
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags)
{
    if (min_vecs > 1) return -ENOSPC;                   /* one vector per device */
    if (!max_vecs || min_vecs > max_vecs) return -EINVAL;
    if (!dev->irq) lkpi_pci_irq_setup(dev);
    struct irq_desc *desc = irq_to_desc(dev->irq);
    if (!desc) return -ENOSPC;
    if (desc->vector >= 0) {
        if (desc->auto_enabled) { desc->auto_enabled = false; return 1; }
        return -EINVAL;                                 /* already allocated */
    }
    int r = msi_on(dev, desc, flags);
    if (r) {
        if (flags & PCI_IRQ_INTX)
            dev_err(&dev->dev, "legacy INTx interrupts are not routed by Kestrel (no MSI here)\n");
        return -ENOSPC;
    }
    return 1;
}

void pci_free_irq_vectors(struct pci_dev *dev)
{
    struct irq_desc *desc = irq_to_desc(dev->irq);
    if (!desc) return;
    if (desc->actions) dev_warn(&dev->dev, "freeing vectors with handlers still installed\n");
    msi_off(dev, desc);
}

int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
    if (nr || !dev->irq) return -EINVAL;
    return (int)dev->irq;
}

int pci_enable_msi(struct pci_dev *dev)
{
    int r = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
    return r < 0 ? r : 0;
}

void pci_disable_msi(struct pci_dev *dev) { pci_free_irq_vectors(dev); }

int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries, int minvec, int maxvec)
{
    int r = pci_alloc_irq_vectors(dev, (unsigned)minvec, (unsigned)maxvec, PCI_IRQ_MSIX);
    if (r < 0) return r;
    if (entries) entries[0].vector = dev->irq;
    return r;
}

void pci_disable_msix(struct pci_dev *dev) { pci_free_irq_vectors(dev); }

/* unbind: the vector goes back to Kestrel, the number stays with the device */
void lkpi_pci_irq_release(struct pci_dev *pdev)
{
    struct irq_desc *desc = irq_to_desc(pdev->irq);
    if (!desc) return;
    while (desc->actions) {
        dev_warn(&pdev->dev, "driver left IRQ %u (%s) requested\n", pdev->irq, desc->actions->name);
        free_irq(pdev->irq, desc->actions->dev_id);
    }
    msi_off(pdev, desc);
    desc->auto_enabled = false;
}

/* ---- request_irq and friends ----------------------------------------------------- */
static irqreturn_t default_primary(int irq, void *dev_id) { return IRQ_WAKE_THREAD; }

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                         unsigned long flags, const char *name, void *dev_id)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long f;

    if (!desc) return -EINVAL;
    if (!handler && !thread_fn) return -EINVAL;
    if ((flags & IRQF_SHARED) && !dev_id) return -EINVAL;
    if (desc->actions && !((flags & IRQF_SHARED) && (desc->actions->flags & IRQF_SHARED))) return -EBUSY;

    if (desc->vector < 0) {                             /* nobody asked for vectors: MSI now */
        int r = msi_on(desc->pdev, desc, PCI_IRQ_MSI | PCI_IRQ_MSIX);
        if (r) {
            dev_err(&desc->pdev->dev, "no MSI, and legacy INTx interrupts are not routed by Kestrel\n");
            return -ENOTSUPP;
        }
        desc->auto_enabled = true;
    }

    struct irqaction *a = kzalloc(sizeof *a, GFP_KERNEL);
    if (!a) return -ENOMEM;
    a->handler = handler ? handler : default_primary;
    a->thread_fn = thread_fn;
    a->dev_id = dev_id;
    a->irq = irq;
    a->flags = flags;
    strscpy(a->name, name ? name : "", sizeof a->name);
    if (thread_fn) {
        a->thread = kthread_create(irq_thread, a, "irq/%u-%s", irq, name ? name : "");
        if (IS_ERR(a->thread)) {
            kfree(a);
            return -ENOMEM;
        }
        wake_up_process(a->thread);
    }

    raw_spin_lock_irqsave(&desc->lock, f);
    struct irqaction **p = &desc->actions;
    while (*p) p = &(*p)->next;
    *p = a;
    if (flags & IRQF_NO_AUTOEN) desc->depth++;
    raw_spin_unlock_irqrestore(&desc->lock, f);
    return 0;
}

void synchronize_irq(unsigned int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    if (!desc) return;
    while (atomic_read(&desc->in_progress)) cpu_relax();
    for (struct irqaction *a = desc->actions; a; a = a->next)
        while (test_bit(0, &a->thread_flags) && a->thread) lkpi_task_yield();
}

const void *free_irq(unsigned int irq, void *dev_id)
{
    struct irq_desc *desc = irq_to_desc(irq);
    struct irqaction *a = NULL;
    unsigned long f;
    if (!desc) return NULL;

    raw_spin_lock_irqsave(&desc->lock, f);
    for (struct irqaction **p = &desc->actions; *p; p = &(*p)->next) {
        if ((*p)->dev_id == dev_id) {
            a = *p;
            *p = a->next;
            break;
        }
    }
    raw_spin_unlock_irqrestore(&desc->lock, f);
    if (!a) {
        printk(KERN_ERR "linuxkpi: trying to free already-free IRQ %u\n", irq);
        return NULL;
    }
    while (atomic_read(&desc->in_progress)) cpu_relax();
    if (a->thread) kthread_stop(a->thread);
    if (!desc->actions && desc->auto_enabled) {
        msi_off(desc->pdev, desc);
        desc->auto_enabled = false;
    }
    /* Linux returns the devname given to request_irq; ours is a copy inside a */
    kfree(a);
    return NULL;
}

void disable_irq_nosync(unsigned int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long f;
    if (!desc) return;
    raw_spin_lock_irqsave(&desc->lock, f);
    desc->depth++;
    raw_spin_unlock_irqrestore(&desc->lock, f);
}

void disable_irq(unsigned int irq)
{
    disable_irq_nosync(irq);
    synchronize_irq(irq);
}

void enable_irq(unsigned int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long f;
    if (!desc) return;
    raw_spin_lock_irqsave(&desc->lock, f);
    if (desc->depth) desc->depth--;
    else printk(KERN_WARNING "linuxkpi: unbalanced enable for IRQ %u\n", irq);
    raw_spin_unlock_irqrestore(&desc->lock, f);
}

struct irq_devres { unsigned int irq; void *dev_id; };

static void devm_irq_release(struct device *dev, void *res)
{
    struct irq_devres *r = res;
    free_irq(r->irq, r->dev_id);
}

static int devm_irq_match(struct device *dev, void *res, void *data)
{
    struct irq_devres *r = res, *m = data;
    return r->irq == m->irq && r->dev_id == m->dev_id;
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                              unsigned long flags, const char *name, void *dev_id)
{
    struct irq_devres *r = devres_alloc(devm_irq_release, sizeof *r, GFP_KERNEL);
    if (!r) return -ENOMEM;
    if (!name) name = dev_name(dev);
    int rc = request_threaded_irq(irq, handler, thread_fn, flags, name, dev_id);
    if (rc) { devres_free(r); return rc; }
    r->irq = irq;
    r->dev_id = dev_id;
    devres_add(dev, r);
    return 0;
}

void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
    struct irq_devres m = { irq, dev_id };
    if (devres_release(dev, devm_irq_release, devm_irq_match, &m))
        printk(KERN_WARNING "linuxkpi: devm_free_irq of IRQ %u that was not managed\n", irq);
}

int pci_request_irq(struct pci_dev *dev, unsigned int nr, irq_handler_t handler, irq_handler_t thread_fn,
                    void *dev_id, const char *fmt, ...)
{
    char name[32];
    va_list ap;
    va_start(ap, fmt);
    vscnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    unsigned long flags = IRQF_SHARED;
    if (!handler) flags |= IRQF_ONESHOT;
    int irq = pci_irq_vector(dev, nr);
    if (irq < 0) return irq;
    return request_threaded_irq((unsigned)irq, handler, thread_fn, flags, name, dev_id);
}

void pci_free_irq(struct pci_dev *dev, unsigned int nr, void *dev_id)
{
    int irq = pci_irq_vector(dev, nr);
    if (irq >= 0) free_irq((unsigned)irq, dev_id);
}

/* ---- tasklets: run by one kernel thread, in the order scheduled ----------------- */
static LIST_HEAD(tasklets);
static DEFINE_SPINLOCK(tasklet_lock);
static struct task_struct *tasklet_task;

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data)
{
    INIT_LIST_HEAD(&t->entry);
    t->state = 0;
    atomic_set(&t->count, 0);
    t->use_callback = false;
    t->func = func;
    t->data = data;
}

void tasklet_setup(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *))
{
    INIT_LIST_HEAD(&t->entry);
    t->state = 0;
    atomic_set(&t->count, 0);
    t->use_callback = true;
    t->callback = callback;
    t->data = 0;
}

void tasklet_schedule(struct tasklet_struct *t)
{
    unsigned long flags;
    if (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) return;
    spin_lock_irqsave(&tasklet_lock, flags);
    list_add_tail(&t->entry, &tasklets);
    spin_unlock_irqrestore(&tasklet_lock, flags);
    if (tasklet_task) wake_up_process(tasklet_task);
}

static int tasklet_thread(void *unused)
{
    for (;;) {
        unsigned long flags;
        set_current_state(TASK_INTERRUPTIBLE);
        spin_lock_irqsave(&tasklet_lock, flags);
        struct tasklet_struct *t = NULL, *pos;
        list_for_each_entry(pos, &tasklets, entry) {
            if (!atomic_read(&pos->count)) { t = pos; break; }
        }
        bool disabled_only = !t && !list_empty(&tasklets);
        if (t) list_del_init(&t->entry);
        spin_unlock_irqrestore(&tasklet_lock, flags);
        if (!t) {
            schedule_timeout(disabled_only ? 1 : MAX_SCHEDULE_TIMEOUT);
            continue;
        }
        __set_current_state(TASK_RUNNING);
        set_bit(TASKLET_STATE_RUN, &t->state);
        clear_bit(TASKLET_STATE_SCHED, &t->state);
        if (t->use_callback) t->callback(t);
        else t->func(t->data);
        clear_bit(TASKLET_STATE_RUN, &t->state);
    }
    return 0;
}

static void tasklet_unlock_wait(struct tasklet_struct *t)
{
    while (test_bit(TASKLET_STATE_RUN, &t->state)) {
        if (in_atomic()) cpu_relax();
        else lkpi_task_yield();
    }
}

void tasklet_disable(struct tasklet_struct *t)
{
    tasklet_disable_nosync(t);
    tasklet_unlock_wait(t);
}

void tasklet_kill(struct tasklet_struct *t)
{
    while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
        msleep(1);
    tasklet_unlock_wait(t);
    clear_bit(TASKLET_STATE_SCHED, &t->state);
}

void lkpi_tasklet_core_init(void)
{
    struct task_struct *t = kthread_run(tasklet_thread, NULL, "lkpi-tasklet");
    if (IS_ERR(t)) lkpi_die("linuxkpi: cannot start the tasklet thread");
    tasklet_task = t;
}
