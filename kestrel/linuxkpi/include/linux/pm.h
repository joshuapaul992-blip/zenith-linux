/* linux/pm.h -- Kestrel's Linux KPI: power management callbacks (never called: no suspend) */
#ifndef _LKPI_LINUX_PM_H
#define _LKPI_LINUX_PM_H

#include <linux/types.h>

struct device;
typedef struct pm_message { int event; } pm_message_t;
#define PMSG_SUSPEND    ((struct pm_message){ .event = 2 })
#define PMSG_RESUME     ((struct pm_message){ .event = 16 })
#define PMSG_FREEZE     ((struct pm_message){ .event = 1 })

struct dev_pm_ops {
    int (*prepare)(struct device *dev);
    void (*complete)(struct device *dev);
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
    int (*freeze)(struct device *dev);
    int (*thaw)(struct device *dev);
    int (*poweroff)(struct device *dev);
    int (*restore)(struct device *dev);
    int (*suspend_late)(struct device *dev);
    int (*resume_early)(struct device *dev);
    int (*freeze_late)(struct device *dev);
    int (*thaw_early)(struct device *dev);
    int (*poweroff_late)(struct device *dev);
    int (*restore_early)(struct device *dev);
    int (*suspend_noirq)(struct device *dev);
    int (*resume_noirq)(struct device *dev);
    int (*freeze_noirq)(struct device *dev);
    int (*thaw_noirq)(struct device *dev);
    int (*poweroff_noirq)(struct device *dev);
    int (*restore_noirq)(struct device *dev);
    int (*runtime_suspend)(struct device *dev);
    int (*runtime_resume)(struct device *dev);
    int (*runtime_idle)(struct device *dev);
};

struct dev_pm_info { unsigned int can_wakeup:1; unsigned int async_suspend:1; };

#define SET_SYSTEM_SLEEP_PM_OPS(s, r) \
    .suspend = s, .resume = r, .freeze = s, .thaw = r, .poweroff = s, .restore = r,
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(s, r)      .suspend_late = s, .resume_early = r,
#define SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r)     .suspend_noirq = s, .resume_noirq = r,
#define SET_RUNTIME_PM_OPS(s, r, i)             .runtime_suspend = s, .runtime_resume = r, .runtime_idle = i,
#define SYSTEM_SLEEP_PM_OPS(s, r)               SET_SYSTEM_SLEEP_PM_OPS(s, r)
#define RUNTIME_PM_OPS(s, r, i)                 SET_RUNTIME_PM_OPS(s, r, i)
#define SIMPLE_DEV_PM_OPS(name, s, r)           const struct dev_pm_ops __maybe_unused name = { SET_SYSTEM_SLEEP_PM_OPS(s, r) }
#define DEFINE_SIMPLE_DEV_PM_OPS(name, s, r)    const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(s, r) }
#define UNIVERSAL_DEV_PM_OPS(name, s, r, i)     const struct dev_pm_ops __maybe_unused name = { \
                                                SET_SYSTEM_SLEEP_PM_OPS(s, r) SET_RUNTIME_PM_OPS(s, r, i) }
#define DEFINE_RUNTIME_DEV_PM_OPS(name, s, r, i) const struct dev_pm_ops name = { RUNTIME_PM_OPS(s, r, i) }
#define pm_ptr(p)                               (p)
#define pm_sleep_ptr(p)                         (p)
#define device_set_wakeup_capable(d, v)         do { } while (0)
#define device_set_wakeup_enable(d, v)          0
#define device_init_wakeup(d, v)                0
#define device_may_wakeup(d)                    0
#define device_wakeup_enable(d)                 0
#define device_wakeup_disable(d)                0

#endif
