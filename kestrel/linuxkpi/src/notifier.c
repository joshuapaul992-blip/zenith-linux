/* linuxkpi/src/notifier.c -- notifier chains, panic, reboot and power-off handlers
 *
 * Chains are lists ordered by priority (highest first). Kestrel's panic()
 * calls lkpi_notifier_panic() once, with interrupts off and the other
 * CPUs stopped: it runs panic_notifier_list (pvpanic tells the host from
 * there). Kestrel's power_off/reboot/halt call lkpi_notifier_power()
 * first: the reboot notifiers run, then the sys-off handlers of that
 * kind, before Kestrel does the deed itself. */
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/reboot.h>
#include <linux/rcupdate.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include "internal.h"

static int chain_add(struct notifier_block **nl, struct notifier_block *n)
{
    while (*nl) {
        if (*nl == n) {
            printk(KERN_WARNING "linuxkpi: notifier %ps registered twice\n", n->notifier_call);
            return -EEXIST;
        }
        if (n->priority > (*nl)->priority) break;
        nl = &(*nl)->next;
    }
    n->next = *nl;
    rcu_assign_pointer(*nl, n);
    return 0;
}

static int chain_remove(struct notifier_block **nl, struct notifier_block *n)
{
    for (; *nl; nl = &(*nl)->next) {
        if (*nl == n) {
            rcu_assign_pointer(*nl, n->next);
            return 0;
        }
    }
    return -ENOENT;
}

static int chain_call(struct notifier_block **nl, unsigned long val, void *v)
{
    int ret = NOTIFY_DONE;
    struct notifier_block *nb = rcu_dereference_raw(*nl);
    while (nb) {
        struct notifier_block *next = rcu_dereference_raw(nb->next);
        ret = nb->notifier_call(nb, val, v);
        if (ret & NOTIFY_STOP_MASK) break;
        nb = next;
    }
    return ret;
}

int atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *nb)
{
    unsigned long flags;
    spin_lock_irqsave(&nh->lock, flags);
    int r = chain_add(&nh->head, nb);
    spin_unlock_irqrestore(&nh->lock, flags);
    return r;
}

int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *nb)
{
    unsigned long flags;
    spin_lock_irqsave(&nh->lock, flags);
    int r = chain_remove(&nh->head, nb);
    spin_unlock_irqrestore(&nh->lock, flags);
    synchronize_rcu();
    return r;
}

int atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v)
{
    rcu_read_lock();
    int r = chain_call(&nh->head, val, v);
    rcu_read_unlock();
    return r;
}

int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *nb)
{
    down_write(&nh->rwsem);
    int r = chain_add(&nh->head, nb);
    up_write(&nh->rwsem);
    return r;
}

int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *nb)
{
    down_write(&nh->rwsem);
    int r = chain_remove(&nh->head, nb);
    up_write(&nh->rwsem);
    return r;
}

int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v)
{
    int r = NOTIFY_DONE;
    if (READ_ONCE(nh->head)) {
        down_read(&nh->rwsem);
        r = chain_call(&nh->head, val, v);
        up_read(&nh->rwsem);
    }
    return r;
}

int raw_notifier_chain_register(struct raw_notifier_head *nh, struct notifier_block *nb) { return chain_add(&nh->head, nb); }
int raw_notifier_chain_unregister(struct raw_notifier_head *nh, struct notifier_block *nb) { return chain_remove(&nh->head, nb); }
int raw_notifier_call_chain(struct raw_notifier_head *nh, unsigned long val, void *v) { return chain_call(&nh->head, val, v); }

/* ---- the system's chains ---------------------------------------------------------- */
ATOMIC_NOTIFIER_HEAD(panic_notifier_list);
bool crash_kexec_post_notifiers;
enum reboot_mode reboot_mode = REBOOT_COLD;

static BLOCKING_NOTIFIER_HEAD(reboot_notifier_list);
static ATOMIC_NOTIFIER_HEAD(restart_handler_list);
static BLOCKING_NOTIFIER_HEAD(power_off_prep_list);
static ATOMIC_NOTIFIER_HEAD(power_off_list);
static BLOCKING_NOTIFIER_HEAD(restart_prep_list);

int register_reboot_notifier(struct notifier_block *nb) { return blocking_notifier_chain_register(&reboot_notifier_list, nb); }
int unregister_reboot_notifier(struct notifier_block *nb) { return blocking_notifier_chain_unregister(&reboot_notifier_list, nb); }
int register_restart_handler(struct notifier_block *nb) { return atomic_notifier_chain_register(&restart_handler_list, nb); }
int unregister_restart_handler(struct notifier_block *nb) { return atomic_notifier_chain_unregister(&restart_handler_list, nb); }

static void devm_reboot_release(struct device *dev, void *res)
{
    unregister_reboot_notifier(*(struct notifier_block **)res);
}

int devm_register_reboot_notifier(struct device *dev, struct notifier_block *nb)
{
    struct notifier_block **r = devres_alloc(devm_reboot_release, sizeof *r, GFP_KERNEL);
    if (!r) return -ENOMEM;
    int ret = register_reboot_notifier(nb);
    if (ret) { devres_free(r); return ret; }
    *r = nb;
    devres_add(dev, r);
    return 0;
}

struct sys_off_handler {
    struct notifier_block   nb;
    int                   (*callback)(struct sys_off_data *data);
    void                   *cb_data;
    enum sys_off_mode       mode;
    struct device          *dev;
};

static int sys_off_notify(struct notifier_block *nb, unsigned long mode, void *cmd)
{
    struct sys_off_handler *h = container_of(nb, struct sys_off_handler, nb);
    struct sys_off_data data = { .mode = (int)mode, .cb_data = h->cb_data, .cmd = cmd, .dev = h->dev };
    return h->callback(&data);
}

static int sys_off_chain(enum sys_off_mode mode, struct sys_off_handler *h, bool add)
{
    switch (mode) {
    case SYS_OFF_MODE_POWER_OFF_PREPARE:
        return add ? blocking_notifier_chain_register(&power_off_prep_list, &h->nb)
                   : blocking_notifier_chain_unregister(&power_off_prep_list, &h->nb);
    case SYS_OFF_MODE_POWER_OFF:
        return add ? atomic_notifier_chain_register(&power_off_list, &h->nb)
                   : atomic_notifier_chain_unregister(&power_off_list, &h->nb);
    case SYS_OFF_MODE_RESTART_PREPARE:
        return add ? blocking_notifier_chain_register(&restart_prep_list, &h->nb)
                   : blocking_notifier_chain_unregister(&restart_prep_list, &h->nb);
    case SYS_OFF_MODE_RESTART:
        return add ? atomic_notifier_chain_register(&restart_handler_list, &h->nb)
                   : atomic_notifier_chain_unregister(&restart_handler_list, &h->nb);
    }
    return -EINVAL;
}

struct sys_off_handler *register_sys_off_handler(enum sys_off_mode mode, int priority,
                                                 int (*callback)(struct sys_off_data *data), void *cb_data)
{
    struct sys_off_handler *h = kzalloc(sizeof *h, GFP_KERNEL);
    if (!h) return ERR_PTR(-ENOMEM);
    h->nb.notifier_call = sys_off_notify;
    h->nb.priority = priority;
    h->callback = callback;
    h->cb_data = cb_data;
    h->mode = mode;
    int r = sys_off_chain(mode, h, true);
    if (r) { kfree(h); return ERR_PTR(r); }
    return h;
}

void unregister_sys_off_handler(struct sys_off_handler *h)
{
    if (IS_ERR_OR_NULL(h)) return;
    sys_off_chain(h->mode, h, false);
    kfree(h);
}

static void devm_sys_off_release(void *data) { unregister_sys_off_handler(data); }

int devm_register_sys_off_handler(struct device *dev, enum sys_off_mode mode, int priority,
                                  int (*callback)(struct sys_off_data *data), void *cb_data)
{
    struct sys_off_handler *h = register_sys_off_handler(mode, priority, callback, cb_data);
    if (IS_ERR(h)) return (int)PTR_ERR(h);
    h->dev = dev;
    return devm_add_action_or_reset(dev, devm_sys_off_release, h);
}

int devm_register_power_off_handler(struct device *dev, int (*callback)(struct sys_off_data *data), void *cb_data)
{
    return devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT, callback, cb_data);
}

int devm_register_restart_handler(struct device *dev, int (*callback)(struct sys_off_data *data), void *cb_data)
{
    return devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_DEFAULT, callback, cb_data);
}

/* ---- Kestrel's side ------------------------------------------------------------------- */
void lkpi_notifier_panic(const char *msg)
{
    if (!READ_ONCE(panic_notifier_list.head)) return;
    printk(KERN_EMERG "linuxkpi: panic: calling the panic notifiers\n");
    atomic_notifier_call_chain(&panic_notifier_list, 0, (void *)msg);
}

void lkpi_notifier_power(int what)
{
    if (what == 1) {                                    /* reboot */
        blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART, NULL);
        blocking_notifier_call_chain(&restart_prep_list, SYS_OFF_MODE_RESTART_PREPARE, NULL);
        atomic_notifier_call_chain(&restart_handler_list, SYS_OFF_MODE_RESTART, NULL);
    } else if (what == 2) {                             /* halt */
        blocking_notifier_call_chain(&reboot_notifier_list, SYS_HALT, NULL);
    } else {                                            /* power off */
        blocking_notifier_call_chain(&reboot_notifier_list, SYS_POWER_OFF, NULL);
        blocking_notifier_call_chain(&power_off_prep_list, SYS_OFF_MODE_POWER_OFF_PREPARE, NULL);
        atomic_notifier_call_chain(&power_off_list, SYS_OFF_MODE_POWER_OFF, NULL);
    }
}

void kernel_restart(char *cmd) { lkpi_power(1); }
void kernel_halt(void) { lkpi_power(2); }
void kernel_power_off(void) { lkpi_power(0); }
void emergency_restart(void) { lkpi_power(1); }
