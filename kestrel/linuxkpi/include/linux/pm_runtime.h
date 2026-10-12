/* linux/pm_runtime.h -- Kestrel's Linux KPI: runtime PM, devices are always on */
#ifndef _LKPI_LINUX_PM_RUNTIME_H
#define _LKPI_LINUX_PM_RUNTIME_H
#include <linux/pm.h>
struct device;
static inline void pm_runtime_enable(struct device *d) { }
static inline void pm_runtime_disable(struct device *d) { }
static inline int devm_pm_runtime_enable(struct device *d) { return 0; }
static inline int pm_runtime_get_sync(struct device *d) { return 1; }
static inline int pm_runtime_resume_and_get(struct device *d) { return 0; }
static inline int pm_runtime_get(struct device *d) { return 1; }
static inline int pm_runtime_get_if_in_use(struct device *d) { return 1; }
static inline int pm_runtime_get_if_active(struct device *d) { return 1; }
static inline void pm_runtime_get_noresume(struct device *d) { }
static inline int pm_runtime_put(struct device *d) { return 0; }
static inline int pm_runtime_put_sync(struct device *d) { return 0; }
static inline int pm_runtime_put_autosuspend(struct device *d) { return 0; }
static inline int pm_runtime_put_sync_autosuspend(struct device *d) { return 0; }
static inline void pm_runtime_put_noidle(struct device *d) { }
static inline void pm_runtime_mark_last_busy(struct device *d) { }
static inline void pm_runtime_use_autosuspend(struct device *d) { }
static inline void pm_runtime_dont_use_autosuspend(struct device *d) { }
static inline void pm_runtime_set_autosuspend_delay(struct device *d, int ms) { }
static inline int pm_runtime_set_active(struct device *d) { return 0; }
static inline void pm_runtime_set_suspended(struct device *d) { }
static inline void pm_runtime_allow(struct device *d) { }
static inline void pm_runtime_forbid(struct device *d) { }
static inline void pm_runtime_no_callbacks(struct device *d) { }
static inline bool pm_runtime_suspended(struct device *d) { return false; }
static inline bool pm_runtime_active(struct device *d) { return true; }
static inline int pm_runtime_resume(struct device *d) { return 0; }
static inline int pm_runtime_suspend(struct device *d) { return 0; }
static inline int pm_runtime_idle(struct device *d) { return 0; }
#endif
