/* linux/cdev.h -- Kestrel's Linux KPI: character devices; cdev_add() makes the /dev node */
#ifndef _LKPI_LINUX_CDEV_H
#define _LKPI_LINUX_CDEV_H
#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>
struct file_operations;
struct device;
struct module;
struct cdev {
    struct kobject                  kobj;
    struct module                  *owner;
    const struct file_operations   *ops;
    struct list_head                list;
    dev_t                           dev;
    unsigned int                    count;
    const char                     *lkpi_name;     /* /dev name, see cdev_device_add */
    void                           *lkpi_nodes[8];
};
void cdev_init(struct cdev *cdev, const struct file_operations *fops);
struct cdev *cdev_alloc(void);
int  cdev_add(struct cdev *p, dev_t dev, unsigned count);
void cdev_del(struct cdev *p);
int  cdev_device_add(struct cdev *cdev, struct device *dev);
void cdev_device_del(struct cdev *cdev, struct device *dev);
static inline void cdev_set_parent(struct cdev *p, struct kobject *kobj) { }
#endif
