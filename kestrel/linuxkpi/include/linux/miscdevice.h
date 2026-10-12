/* linux/miscdevice.h -- Kestrel's Linux KPI: single-node character devices (major 10) */
#ifndef _LKPI_LINUX_MISCDEVICE_H
#define _LKPI_LINUX_MISCDEVICE_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>

#define MISC_MAJOR              10
#define WATCHDOG_MINOR          130
#define TEMP_MINOR              131
#define HPET_MINOR              228
#define MISC_DYNAMIC_MINOR      255

struct device;
struct attribute_group;

struct miscdevice {
    int                             minor;
    const char                     *name;
    const struct file_operations   *fops;
    struct list_head                list;
    struct device                  *parent;
    struct device                  *this_device;
    const struct attribute_group  **groups;
    const char                     *nodename;
    umode_t                         mode;
    void                           *lkpi_node;
};

int  misc_register(struct miscdevice *misc);
void misc_deregister(struct miscdevice *misc);
#define module_misc_device(__misc_device) module_driver(__misc_device, misc_register, misc_deregister)

#endif
