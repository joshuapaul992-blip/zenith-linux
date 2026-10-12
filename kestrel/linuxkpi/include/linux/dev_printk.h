/* linux/dev_printk.h -- Kestrel's Linux KPI: messages about a device,
 * "<driver> <device>: message" as in Linux (linuxkpi/src/printk.c) */
#ifndef _LKPI_LINUX_DEV_PRINTK_H
#define _LKPI_LINUX_DEV_PRINTK_H

#include <linux/printk.h>
#include <linux/kern_levels.h>
#include <linux/bug.h>

struct device;

__printf(3, 4) void _dev_printk(const char *level, const struct device *dev, const char *fmt, ...);
#define dev_printk(level, dev, fmt, ...)  _dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...)        _dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)        _dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)         _dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)          _dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)         _dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...)       _dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)         _dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define dev_dbg(dev, fmt, ...)          _dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...)          ({ if (0) _dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); })
#endif
#define dev_vdbg(dev, fmt, ...)         dev_dbg(dev, fmt, ##__VA_ARGS__)
#define __lkpi_dev_once(fn, dev, fmt, ...) \
    do { static bool __print_once; if (!__print_once) { __print_once = true; fn(dev, fmt, ##__VA_ARGS__); } } while (0)
#define dev_err_once(dev, fmt, ...)     __lkpi_dev_once(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...)    __lkpi_dev_once(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...)    __lkpi_dev_once(dev_info, dev, fmt, ##__VA_ARGS__)
#define dev_notice_once(dev, fmt, ...)  __lkpi_dev_once(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_dbg_once(dev, fmt, ...)     dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...)    dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)   dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)   dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_notice_ratelimited(dev, fmt, ...) dev_notice(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)    dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_WARN(dev, fmt, ...)         WARN(1, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, cond, fmt, ...) WARN_ONCE(cond, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
/* log unless err is -EPROBE_DEFER; returns err */
__printf(3, 4) int dev_err_probe(const struct device *dev, int err, const char *fmt, ...);
#define dev_warn_probe(dev, err, fmt, ...) dev_err_probe(dev, err, fmt, ##__VA_ARGS__)

#endif
