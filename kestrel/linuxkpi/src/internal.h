/* linuxkpi/src/internal.h -- shared between the KPI's own source files
 *
 * Everything here is compiled against the KPI's <linux/...> headers, like
 * a Linux driver; Kestrel is reached only through <lkpi/glue.h>. */
#ifndef LKPI_INTERNAL_H
#define LKPI_INTERNAL_H

#include <linux/types.h>
#include <linux/list.h>
#include <lkpi/glue.h>

#define LKPI_MAX_CPUS   64              /* per-CPU arrays (Kestrel has at most 16) */

struct task_struct;
struct device;
struct pci_dev;
struct bus_type;

/* sched.c */
void lkpi_sched_init(void);
void lkpi_set_current(struct task_struct *ts);     /* this thread is ts from now on  */
void lkpi_clear_current(void);                      /* ... until it exits             */
void lkpi_hardirq_enter(void);
void lkpi_hardirq_exit(void);
bool lkpi_debug_enabled(void);

/* timer.c, workqueue.c, irq.c (tasklets): the threads behind them */
void lkpi_timer_core_init(void);
void lkpi_workqueue_core_init(void);
void lkpi_tasklet_core_init(void);

/* device.c */
void lkpi_device_core_init(void);
int  lkpi_bus_add(const struct bus_type *bus);
void lkpi_deferred_probe_flush(void);

/* pci.c, irq.c */
void lkpi_pci_core_init(void);
void lkpi_pci_irq_setup(struct pci_dev *pdev);      /* at enable: a lazy irq for the device */
void lkpi_pci_irq_release(struct pci_dev *pdev);    /* at unbind: vectors back            */

/* chardev.c, watchdog.c, notifier.c */
void *lkpi_devnode_add(struct device *dev);
void lkpi_devnode_remove(struct device *dev);
void lkpi_notifier_panic(const char *msg);
void lkpi_notifier_power(int what);

/* module.c */
bool lkpi_cmdline_get(const char *key, char *buf, size_t n);  /* "key=value" on the command line */
bool lkpi_cmdline_flag(const char *key);            /* key=1/y/on present */

/* selftest.c */
void lkpi_selftest_run(void);

#endif
