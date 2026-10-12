/* linuxkpi/src/watchdog.c -- a minimal watchdog core and /dev/watchdog
 *
 * A driver registers a struct watchdog_device; the core gives it
 * /dev/watchdogN (and /dev/watchdog, misc 10:130, to the first one) with
 * Linux's user interface:
 *   - opening starts the watchdog (only one opener at a time);
 *   - any write, or WDIOC_KEEPALIVE, pings it; a 'V' in the data written
 *     arms the "magic close";
 *   - closing after the magic character stops it; closing without it
 *     leaves it running ("watchdog did not stop!"), so the machine resets
 *     when the timeout passes, unless someone opens it again and pings;
 *   - WDIOC_GETSUPPORT, GETSTATUS, GETBOOTSTATUS, SETOPTIONS, SETTIMEOUT,
 *     GETTIMEOUT, SETPRETIMEOUT, GETPRETIMEOUT and GETTIMELEFT as in Linux.
 * The core pings by itself only for hardware that cannot be stopped
 * (WDOG_HW_RUNNING) while nobody has the device open, or whose hardware
 * limit (max_hw_heartbeat_ms) is shorter than the timeout asked for.
 * There are no pretimeout governors and no sysfs attributes. */
#include <linux/kernel.h>
#include <linux/watchdog.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/reboot.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/printk.h>
#include "internal.h"

#define MAX_DOGS                32

/* wd_data->status */
#define WD_DEV_OPEN             0
#define WD_ALLOW_RELEASE        1       /* the magic character was written */
#define WD_KEEPALIVE            2       /* pinged since the last GETSTATUS */

struct watchdog_core_data {
    struct device           dev;
    struct cdev             cdev;
    struct watchdog_device *wdd;        /* NULL once unregistered */
    struct mutex            lock;
    unsigned long           status;
    unsigned long           last_keepalive;     /* jiffies */
    struct delayed_work     work;
};

static dev_t watchdog_devt;
static DEFINE_IDA(watchdog_ida);
static struct watchdog_core_data *legacy_wd_data;   /* behind /dev/watchdog */
static const struct class watchdog_class = { .name = "watchdog" };

/* ---- the operations, wd_data->lock held ----------------------------------------- */
static bool need_worker(struct watchdog_device *wdd)
{
    unsigned int hm = wdd->max_hw_heartbeat_ms, t = wdd->timeout * 1000;
    return (hm && watchdog_active(wdd) && t > hm) || (t && !watchdog_active(wdd) && watchdog_hw_running(wdd));
}

static void update_worker(struct watchdog_core_data *wd)
{
    struct watchdog_device *wdd = wd->wdd;
    if (!wdd || !need_worker(wdd)) { cancel_delayed_work(&wd->work); return; }
    unsigned int hm = wdd->max_hw_heartbeat_ms ? wdd->max_hw_heartbeat_ms : wdd->timeout * 1000;
    unsigned int every = min(hm, wdd->timeout * 1000) / 2;
    mod_delayed_work(system_wq, &wd->work, msecs_to_jiffies(every ? every : 500));
}

static int hw_ping(struct watchdog_device *wdd)
{
    return wdd->ops->ping ? wdd->ops->ping(wdd) : wdd->ops->start(wdd);
}

static int wd_ping(struct watchdog_core_data *wd)
{
    struct watchdog_device *wdd = wd->wdd;
    if (!watchdog_active(wdd) && !watchdog_hw_running(wdd)) return 0;
    set_bit(WD_KEEPALIVE, &wd->status);
    wd->last_keepalive = jiffies;
    int r = hw_ping(wdd);
    update_worker(wd);
    return r;
}

static int wd_start(struct watchdog_core_data *wd)
{
    struct watchdog_device *wdd = wd->wdd;
    if (watchdog_active(wdd)) return 0;
    set_bit(WD_KEEPALIVE, &wd->status);
    int r = watchdog_hw_running(wdd) && wdd->ops->ping ? wdd->ops->ping(wdd) : wdd->ops->start(wdd);
    if (r) return r;
    set_bit(WDOG_ACTIVE, &wdd->status);
    wd->last_keepalive = jiffies;
    update_worker(wd);
    return 0;
}

static int wd_stop(struct watchdog_core_data *wd)
{
    struct watchdog_device *wdd = wd->wdd;
    if (!watchdog_active(wdd)) return 0;
    if (test_bit(WDOG_NO_WAY_OUT, &wdd->status)) {
        pr_info("watchdog%d: nowayout prevents watchdog being stopped!\n", wdd->id);
        return -EBUSY;
    }
    int r = 0;
    if (wdd->ops->stop) {
        clear_bit(WDOG_HW_RUNNING, &wdd->status);
        r = wdd->ops->stop(wdd);
    } else {
        set_bit(WDOG_HW_RUNNING, &wdd->status);     /* it cannot stop: the core pings it */
    }
    if (!r) {
        clear_bit(WDOG_ACTIVE, &wdd->status);
        update_worker(wd);
    }
    return r;
}

static int wd_set_timeout(struct watchdog_core_data *wd, unsigned int timeout)
{
    struct watchdog_device *wdd = wd->wdd;
    if (!(wdd->info->options & WDIOF_SETTIMEOUT)) return -EOPNOTSUPP;
    if (watchdog_timeout_invalid(wdd, timeout)) return -EINVAL;
    int r = 0;
    if (wdd->ops->set_timeout) {
        r = wdd->ops->set_timeout(wdd, timeout);
    } else {
        wdd->timeout = timeout;
        if (wdd->pretimeout >= wdd->timeout) wdd->pretimeout = 0;
    }
    update_worker(wd);
    return r;
}

static int wd_set_pretimeout(struct watchdog_core_data *wd, unsigned int timeout)
{
    struct watchdog_device *wdd = wd->wdd;
    if (!(wdd->info->options & WDIOF_PRETIMEOUT)) return -EOPNOTSUPP;
    if (watchdog_pretimeout_invalid(wdd, timeout)) return -EINVAL;
    if (wdd->ops->set_pretimeout) return wdd->ops->set_pretimeout(wdd, timeout);
    wdd->pretimeout = timeout;
    return 0;
}

static void worker_fn(struct work_struct *w)
{
    struct watchdog_core_data *wd = container_of(to_delayed_work(w), struct watchdog_core_data, work);
    mutex_lock(&wd->lock);
    struct watchdog_device *wdd = wd->wdd;
    if (wdd && need_worker(wdd)) {
        /* while open, ping only as long as user space keeps pinging */
        bool user_alive = !watchdog_active(wdd) ||
                          time_before(jiffies, wd->last_keepalive + msecs_to_jiffies(wdd->timeout * 1000));
        if (user_alive) hw_ping(wdd);
        update_worker(wd);
    }
    mutex_unlock(&wd->lock);
}

/* ---- the device file ------------------------------------------------------------------ */
static int watchdog_open(struct inode *inode, struct file *file)
{
    struct watchdog_core_data *wd = NULL;
    if (file->private_data) wd = legacy_wd_data;       /* /dev/watchdog: the misc device */
    else if (inode->i_cdev) wd = container_of(inode->i_cdev, struct watchdog_core_data, cdev);
    if (!wd) return -ENODEV;
    if (test_and_set_bit(WD_DEV_OPEN, &wd->status)) return -EBUSY;
    mutex_lock(&wd->lock);
    int r = wd->wdd ? wd_start(wd) : -ENODEV;
    mutex_unlock(&wd->lock);
    if (r) {
        clear_bit(WD_DEV_OPEN, &wd->status);
        return r;
    }
    get_device(&wd->dev);
    file->private_data = wd;
    return stream_open(inode, file);
}

static int watchdog_release(struct inode *inode, struct file *file)
{
    struct watchdog_core_data *wd = file->private_data;
    int r = -EBUSY;
    mutex_lock(&wd->lock);
    struct watchdog_device *wdd = wd->wdd;
    if (!wdd || !watchdog_active(wdd))
        r = 0;
    else if (test_and_clear_bit(WD_ALLOW_RELEASE, &wd->status) || !(wdd->info->options & WDIOF_MAGICCLOSE))
        r = wd_stop(wd);
    if (r < 0) {
        pr_crit("watchdog%d: watchdog did not stop!\n", wdd->id);
        wd_ping(wd);
    }
    if (wdd) update_worker(wd);
    mutex_unlock(&wd->lock);
    clear_bit(WD_DEV_OPEN, &wd->status);
    put_device(&wd->dev);
    return 0;
}

static ssize_t watchdog_write(struct file *file, const char __user *data, size_t len, loff_t *ppos)
{
    struct watchdog_core_data *wd = file->private_data;
    if (!len) return 0;
    clear_bit(WD_ALLOW_RELEASE, &wd->status);
    mutex_lock(&wd->lock);
    struct watchdog_device *wdd = wd->wdd;
    if (!wdd) { mutex_unlock(&wd->lock); return -ENODEV; }
    if (!test_bit(WDOG_NO_WAY_OUT, &wdd->status)) {
        for (size_t i = 0; i < len; i++) {
            char c;
            if (get_user(c, data + i)) { mutex_unlock(&wd->lock); return -EFAULT; }
            if (c == 'V') set_bit(WD_ALLOW_RELEASE, &wd->status);
        }
    }
    int r = wd_ping(wd);
    mutex_unlock(&wd->lock);
    return r < 0 ? r : (ssize_t)len;
}

static long watchdog_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct watchdog_core_data *wd = file->private_data;
    void __user *argp = (void __user *)arg;
    int __user *p = argp;
    unsigned int val;
    long r = 0;

    mutex_lock(&wd->lock);
    struct watchdog_device *wdd = wd->wdd;
    if (!wdd) { r = -ENODEV; goto out; }
    if (wdd->ops->ioctl) {                              /* the driver's own come first */
        r = wdd->ops->ioctl(wdd, cmd, arg);
        if (r != -ENOIOCTLCMD) goto out;
        r = 0;
    }

    switch (cmd) {
    case WDIOC_GETSUPPORT:
        r = copy_to_user(argp, wdd->info, sizeof(struct watchdog_info)) ? -EFAULT : 0;
        break;
    case WDIOC_GETSTATUS:
        val = wdd->ops->status ? wdd->ops->status(wdd) : 0;
        if (test_and_clear_bit(WD_KEEPALIVE, &wd->status)) val |= WDIOF_KEEPALIVEPING;
        if (test_bit(WD_ALLOW_RELEASE, &wd->status)) val |= WDIOF_MAGICCLOSE;
        r = put_user(val, p);
        break;
    case WDIOC_GETBOOTSTATUS:
        r = put_user(wdd->bootstatus, p);
        break;
    case WDIOC_SETOPTIONS:
        if (get_user(val, p)) { r = -EFAULT; break; }
        if (val & WDIOS_DISABLECARD) { r = wd_stop(wd); if (r < 0) break; }
        if (val & WDIOS_ENABLECARD) r = wd_start(wd);
        break;
    case WDIOC_KEEPALIVE:
        if (!(wdd->info->options & WDIOF_KEEPALIVEPING)) { r = -EOPNOTSUPP; break; }
        r = wd_ping(wd);
        break;
    case WDIOC_SETTIMEOUT:
        if (get_user(val, p)) { r = -EFAULT; break; }
        r = wd_set_timeout(wd, val);
        if (r < 0) break;
        /* the new timeout takes effect with a ping; then report it */
        r = wd_ping(wd);
        if (r < 0) break;
        fallthrough;
    case WDIOC_GETTIMEOUT:
        r = wdd->timeout ? put_user(wdd->timeout, p) : -EOPNOTSUPP;
        break;
    case WDIOC_GETTIMELEFT:
        if (!wdd->ops->get_timeleft) { r = -EOPNOTSUPP; break; }
        r = put_user(wdd->ops->get_timeleft(wdd), p);
        break;
    case WDIOC_SETPRETIMEOUT:
        if (get_user(val, p)) { r = -EFAULT; break; }
        r = wd_set_pretimeout(wd, val);
        break;
    case WDIOC_GETPRETIMEOUT:
        r = put_user(wdd->pretimeout, p);
        break;
    default:
        r = -ENOTTY;
        break;
    }
out:
    mutex_unlock(&wd->lock);
    return r;
}

static const struct file_operations watchdog_fops = {
    .owner = THIS_MODULE,
    .write = watchdog_write,
    .unlocked_ioctl = watchdog_ioctl,
    .open = watchdog_open,
    .release = watchdog_release,
};

static struct miscdevice watchdog_miscdev = {
    .minor = WATCHDOG_MINOR,
    .name = "watchdog",
    .fops = &watchdog_fops,
};

/* ---- registration ------------------------------------------------------------------------- */
static int reboot_notify(struct notifier_block *nb, unsigned long code, void *data)
{
    struct watchdog_device *wdd = container_of(nb, struct watchdog_device, reboot_nb);
    if ((code == SYS_DOWN || code == SYS_HALT || code == SYS_POWER_OFF) &&
        (watchdog_active(wdd) || watchdog_hw_running(wdd)) && wdd->ops->stop) {
        if (wdd->ops->stop(wdd)) pr_warn("watchdog%d: failed to stop on reboot\n", wdd->id);
        else clear_bit(WDOG_ACTIVE, &wdd->status);
    }
    return NOTIFY_DONE;
}

static int restart_notify(struct notifier_block *nb, unsigned long action, void *data)
{
    struct watchdog_device *wdd = container_of(nb, struct watchdog_device, restart_nb);
    return wdd->ops->restart(wdd, action, data) ? NOTIFY_DONE : NOTIFY_STOP;
}

void watchdog_set_restart_priority(struct watchdog_device *wdd, int priority)
{
    wdd->restart_nb.priority = priority;
}

int watchdog_init_timeout(struct watchdog_device *wdd, unsigned int timeout_parm, struct device *dev)
{
    const char *who = wdd->parent ? dev_name(wdd->parent) : (const char *)wdd->info->identity;
    int r = 0;
    if (wdd->max_timeout && wdd->max_timeout < wdd->min_timeout) {
        pr_info("%s: invalid min and max timeout, resetting to 0\n", who);
        wdd->min_timeout = wdd->max_timeout = 0;
    }
    if (timeout_parm) {                                 /* a module parameter, most likely */
        if (!watchdog_timeout_invalid(wdd, timeout_parm)) {
            wdd->timeout = timeout_parm;
            return 0;
        }
        pr_err("%s: driver supplied timeout (%u) out of range\n", who, timeout_parm);
        r = -EINVAL;
    }
    if (r < 0 && wdd->timeout) pr_warn("%s: falling back to default timeout (%u)\n", who, wdd->timeout);
    return r;
}

static void wd_data_release(struct device *dev)
{
    kfree(container_of(dev, struct watchdog_core_data, dev));
}

int watchdog_register_device(struct watchdog_device *wdd)
{
    if (!wdd || !wdd->info || !wdd->ops || !wdd->ops->start || (!wdd->ops->stop && !wdd->max_hw_heartbeat_ms))
        return -EINVAL;
    if (!watchdog_devt) return -ENODEV;
    if (wdd->max_timeout && wdd->max_timeout < wdd->min_timeout) wdd->min_timeout = wdd->max_timeout = 0;

    int id = ida_alloc_max(&watchdog_ida, MAX_DOGS - 1, GFP_KERNEL);
    if (id < 0) return id;
    wdd->id = id;

    struct watchdog_core_data *wd = kzalloc(sizeof *wd, GFP_KERNEL);
    if (!wd) { ida_free(&watchdog_ida, (unsigned)id); return -ENOMEM; }
    wd->wdd = wdd;
    wdd->wd_data = wd;
    mutex_init(&wd->lock);
    INIT_DELAYED_WORK(&wd->work, worker_fn);

    device_initialize(&wd->dev);
    wd->dev.devt = MKDEV(MAJOR(watchdog_devt), id);
    wd->dev.class = &watchdog_class;
    wd->dev.parent = wdd->parent;
    wd->dev.groups = wdd->groups;
    wd->dev.release = wd_data_release;
    dev_set_drvdata(&wd->dev, wdd);
    dev_set_name(&wd->dev, "watchdog%d", id);

    bool legacy = false;
    if (id == 0) {
        legacy_wd_data = wd;
        int r = misc_register(&watchdog_miscdev);
        if (r) {
            pr_err("watchdog%d: cannot register miscdev on minor=%d (err=%d)\n", id, WATCHDOG_MINOR, r);
            legacy_wd_data = NULL;
        } else {
            legacy = true;
        }
    }
    cdev_init(&wd->cdev, &watchdog_fops);
    int r = cdev_device_add(&wd->cdev, &wd->dev);
    if (r) {
        pr_err("watchdog%d: unable to add device %d:%d\n", id, MAJOR(watchdog_devt), id);
        if (legacy) { misc_deregister(&watchdog_miscdev); legacy_wd_data = NULL; }
        ida_free(&watchdog_ida, (unsigned)id);
        wdd->wd_data = NULL;
        put_device(&wd->dev);
        return r;
    }

    if (watchdog_hw_running(wdd)) {
        mutex_lock(&wd->lock);
        hw_ping(wdd);
        update_worker(wd);
        mutex_unlock(&wd->lock);
        pr_info("watchdog%d running and kernel based pre-userspace handler enabled\n", id);
    }
    if (test_bit(WDOG_STOP_ON_REBOOT, &wdd->status)) {
        wdd->reboot_nb.notifier_call = reboot_notify;
        if (register_reboot_notifier(&wdd->reboot_nb))
            pr_warn("watchdog%d: cannot register reboot notifier\n", id);
    }
    if (wdd->ops->restart) {
        wdd->restart_nb.notifier_call = restart_notify;
        register_restart_handler(&wdd->restart_nb);
    }
    pr_info("watchdog%d: %s registered as /dev/watchdog%d%s, timeout %u s\n", id, wdd->info->identity, id,
            legacy ? " and /dev/watchdog" : "", wdd->timeout);
    return 0;
}

void watchdog_unregister_device(struct watchdog_device *wdd)
{
    struct watchdog_core_data *wd = wdd ? wdd->wd_data : NULL;
    if (!wd) return;
    if (wdd->ops->restart) unregister_restart_handler(&wdd->restart_nb);
    if (test_bit(WDOG_STOP_ON_REBOOT, &wdd->status)) unregister_reboot_notifier(&wdd->reboot_nb);

    mutex_lock(&wd->lock);
    if (test_bit(WDOG_STOP_ON_UNREGISTER, &wdd->status) && watchdog_active(wdd)) {
        clear_bit(WDOG_NO_WAY_OUT, &wdd->status);
        wd_stop(wd);
    }
    wd->wdd = NULL;                                     /* open files now get -ENODEV */
    wdd->wd_data = NULL;
    mutex_unlock(&wd->lock);
    cancel_delayed_work_sync(&wd->work);

    if (wd == legacy_wd_data) {
        misc_deregister(&watchdog_miscdev);
        legacy_wd_data = NULL;
    }
    cdev_device_del(&wd->cdev, &wd->dev);
    ida_free(&watchdog_ida, (unsigned)wdd->id);
    put_device(&wd->dev);
}

static void devm_watchdog_unregister(void *wdd) { watchdog_unregister_device(wdd); }

int devm_watchdog_register_device(struct device *dev, struct watchdog_device *wdd)
{
    int r = watchdog_register_device(wdd);
    if (r) return r;
    return devm_add_action_or_reset(dev, devm_watchdog_unregister, wdd);
}

void watchdog_notify_pretimeout(struct watchdog_device *wdd)
{
    pr_alert("watchdog%d: pretimeout event\n", wdd->id);
}

/* before the drivers (module_init is a device initcall) */
static int __init watchdog_core_init(void)
{
    int r = alloc_chrdev_region(&watchdog_devt, 0, MAX_DOGS, "watchdog");
    if (r) pr_err("watchdog: unable to allocate char dev region\n");
    return r;
}
subsys_initcall(watchdog_core_init);
