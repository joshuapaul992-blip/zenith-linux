/* linuxkpi/src/chardev.c -- character devices, misc devices and user memory
 *
 * Linux drivers give the KPI a struct file_operations; Kestrel's devfs
 * node (glue/devfs.c) calls back here, and each open gets a struct file
 * and a struct inode of its own. A device with a dev_t gets its /dev node
 * when it is added (device_create, cdev_device_add, misc_register): that
 * is what udev would do on Linux. Opening the node finds the operations
 * by number: a misc device (major 10), a cdev covering it, or a range
 * from register_chrdev().
 *
 * User memory: read() and write() handlers are given kernel buffers by
 * Kestrel, so around those calls copy_to_user/copy_from_user are plain
 * copies; ioctl arguments are real user addresses, checked by Kestrel. */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include "internal.h"

/* ---- user memory ----------------------------------------------------------- */
static bool kernel_copies(void) { return current->lkpi_uaccess_kernel; }

unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n)
{
    if (!n) return 0;
    if (kernel_copies()) { memcpy(to, (const void __force *)from, n); return 0; }
    return lkpi_copy_from_user_glue(to, (const void __force *)from, n) ? n : 0;
}

unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n)
{
    if (!n) return 0;
    if (kernel_copies()) { memcpy((void __force *)to, from, n); return 0; }
    return lkpi_copy_to_user_glue((void __force *)to, from, n) ? n : 0;
}

unsigned long lkpi_clear_user(void __user *to, unsigned long n)
{
    static const char zeros[256];
    unsigned long done = 0;
    while (done < n) {
        unsigned long k = n - done < sizeof zeros ? n - done : sizeof zeros;
        if (lkpi_copy_to_user((char __user *)to + done, zeros, k)) return n - done;
        done += k;
    }
    return 0;
}

long lkpi_strncpy_from_user(char *dst, const char __user *src, long count)
{
    long i;
    for (i = 0; i < count; i++) {
        if (lkpi_copy_from_user(&dst[i], src + i, 1)) return -EFAULT;
        if (!dst[i]) return i;
    }
    return count;
}

bool lkpi_access_ok(const void __user *addr, unsigned long size)
{
    return kernel_copies() || lkpi_user_range_ok((unsigned long)addr, size);
}

/* ---- device numbers ----------------------------------------------------------- */
struct chrdev {
    struct list_head                node;
    unsigned int                    major, baseminor, count;
    char                            name[32];
    const struct file_operations   *fops;       /* register_chrdev() only */
};

static LIST_HEAD(chrdevs);
static LIST_HEAD(cdevs);                        /* struct cdev, by ->list */
static LIST_HEAD(miscs);                        /* struct miscdevice, by ->list */
static DEFINE_MUTEX(chr_lock);
static const struct class misc_class = { .name = "misc" };

static bool range_busy(unsigned int major, unsigned int base, unsigned int count)
{
    struct chrdev *c;
    list_for_each_entry(c, &chrdevs, node)
        if (c->major == major && base < c->baseminor + c->count && c->baseminor < base + count) return true;
    return false;
}

static bool major_used(unsigned int major)
{
    struct chrdev *c;
    list_for_each_entry(c, &chrdevs, node)
        if (c->major == major) return true;
    return major == MISC_MAJOR;
}

static struct chrdev *add_region(unsigned int major, unsigned int base, unsigned int count, const char *name,
                                 const struct file_operations *fops)
{
    if (!major) {                               /* dynamic: Linux's local range, from the top */
        for (major = 254; major >= 234 && major_used(major); major--) { }
        if (major < 234) return ERR_PTR(-EBUSY);
    }
    if (range_busy(major, base, count)) return ERR_PTR(-EBUSY);
    struct chrdev *c = kzalloc(sizeof *c, GFP_KERNEL);
    if (!c) return ERR_PTR(-ENOMEM);
    c->major = major;
    c->baseminor = base;
    c->count = count;
    c->fops = fops;
    strscpy(c->name, name, sizeof c->name);
    list_add_tail(&c->node, &chrdevs);
    return c;
}

int register_chrdev_region(dev_t from, unsigned count, const char *name)
{
    mutex_lock(&chr_lock);
    struct chrdev *c = add_region(MAJOR(from), MINOR(from), count, name, NULL);
    mutex_unlock(&chr_lock);
    return IS_ERR(c) ? (int)PTR_ERR(c) : 0;
}

int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)
{
    mutex_lock(&chr_lock);
    struct chrdev *c = add_region(0, baseminor, count, name, NULL);
    mutex_unlock(&chr_lock);
    if (IS_ERR(c)) return (int)PTR_ERR(c);
    *dev = MKDEV(c->major, baseminor);
    return 0;
}

static void remove_region(unsigned int major, unsigned int base, unsigned int count)
{
    struct chrdev *c, *n;
    mutex_lock(&chr_lock);
    list_for_each_entry_safe(c, n, &chrdevs, node) {
        if (c->major == major && c->baseminor == base && c->count == count) {
            list_del(&c->node);
            kfree(c);
            break;
        }
    }
    mutex_unlock(&chr_lock);
}

void unregister_chrdev_region(dev_t from, unsigned count) { remove_region(MAJOR(from), MINOR(from), count); }

int __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name,
                      const struct file_operations *fops)
{
    mutex_lock(&chr_lock);
    struct chrdev *c = add_region(major, baseminor, count, name, fops);
    mutex_unlock(&chr_lock);
    if (IS_ERR(c)) return (int)PTR_ERR(c);
    return major ? 0 : (int)c->major;
}

void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name)
{
    remove_region(major, baseminor, count);
}

/* ---- cdevs ------------------------------------------------------------------------ */
void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
    memset(cdev, 0, sizeof *cdev);
    INIT_LIST_HEAD(&cdev->list);
    kref_init(&cdev->kobj.kref);
    cdev->ops = fops;
}

struct cdev *cdev_alloc(void)
{
    struct cdev *p = kzalloc(sizeof *p, GFP_KERNEL);
    if (p) { INIT_LIST_HEAD(&p->list); kref_init(&p->kobj.kref); }
    return p;
}

int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
    p->dev = dev;
    p->count = count;
    mutex_lock(&chr_lock);
    list_add_tail(&p->list, &cdevs);
    mutex_unlock(&chr_lock);
    return 0;
}

void cdev_del(struct cdev *p)
{
    mutex_lock(&chr_lock);
    list_del_init(&p->list);
    mutex_unlock(&chr_lock);
}

int cdev_device_add(struct cdev *cdev, struct device *dev)
{
    if (dev->devt) {
        int r = cdev_add(cdev, dev->devt, 1);
        if (r) return r;
    }
    int r = device_add(dev);
    if (r && dev->devt) cdev_del(cdev);
    return r;
}

void cdev_device_del(struct cdev *cdev, struct device *dev)
{
    device_del(dev);
    if (dev->devt) cdev_del(cdev);
}

/* ---- misc devices ------------------------------------------------------------------ */
#define DYNAMIC_MINOR_FIRST     64
#define DYNAMIC_MINOR_LAST      127

static struct miscdevice *find_misc(unsigned int minor)
{
    struct miscdevice *m;
    list_for_each_entry(m, &miscs, list)
        if ((unsigned int)m->minor == minor) return m;
    return NULL;
}

int misc_register(struct miscdevice *misc)
{
    mutex_lock(&chr_lock);
    if (misc->minor == MISC_DYNAMIC_MINOR) {
        int m;
        for (m = DYNAMIC_MINOR_LAST; m >= DYNAMIC_MINOR_FIRST && find_misc((unsigned)m); m--) { }
        if (m < DYNAMIC_MINOR_FIRST) { mutex_unlock(&chr_lock); return -EBUSY; }
        misc->minor = m;
    } else if (find_misc((unsigned)misc->minor)) {
        mutex_unlock(&chr_lock);
        return -EBUSY;
    }
    list_add_tail(&misc->list, &miscs);
    mutex_unlock(&chr_lock);

    struct device *dev = device_create(&misc_class, misc->parent, MKDEV(MISC_MAJOR, misc->minor), misc,
                                       "%s", misc->nodename ? misc->nodename : misc->name);
    if (IS_ERR(dev)) {
        mutex_lock(&chr_lock);
        list_del(&misc->list);
        mutex_unlock(&chr_lock);
        return (int)PTR_ERR(dev);
    }
    misc->this_device = dev;
    return 0;
}

void misc_deregister(struct miscdevice *misc)
{
    if (!misc->this_device) return;
    device_destroy(&misc_class, MKDEV(MISC_MAJOR, misc->minor));
    misc->this_device = NULL;
    mutex_lock(&chr_lock);
    list_del_init(&misc->list);
    mutex_unlock(&chr_lock);
}

/* ---- the files ------------------------------------------------------------------------ */
struct lkpi_file {
    struct file     file;
    struct inode    inode;
};

/* the operations behind a device number, and the misc device if it is one */
static const struct file_operations *lookup(dev_t devt, struct miscdevice **misc, struct cdev **cdev)
{
    const struct file_operations *fops = NULL;
    *misc = NULL;
    *cdev = NULL;
    mutex_lock(&chr_lock);
    if (MAJOR(devt) == MISC_MAJOR) {
        struct miscdevice *m = find_misc(MINOR(devt));
        if (m) { *misc = m; fops = m->fops; }
    }
    if (!fops) {
        struct cdev *c;
        list_for_each_entry(c, &cdevs, list) {
            if (devt >= c->dev && devt < c->dev + c->count) { *cdev = c; fops = c->ops; break; }
        }
    }
    if (!fops) {
        struct chrdev *r;
        list_for_each_entry(r, &chrdevs, node) {
            if (r->fops && r->major == MAJOR(devt) && MINOR(devt) >= r->baseminor &&
                MINOR(devt) < r->baseminor + r->count) { fops = r->fops; break; }
        }
    }
    mutex_unlock(&chr_lock);
    return fops;
}

struct devnode {
    dev_t           devt;
    void           *node;               /* the glue's handle */
};

static int node_open(void *ctx, int flags, void **out)
{
    struct devnode *dn = ctx;
    struct miscdevice *misc;
    struct cdev *cdev;
    const struct file_operations *fops = lookup(dn->devt, &misc, &cdev);
    if (!fops) return -ENXIO;

    struct lkpi_file *lf = kzalloc(sizeof *lf, GFP_KERNEL);
    if (!lf) return -ENOMEM;
    lf->inode.i_mode = S_IFCHR | 0600;
    lf->inode.i_rdev = dn->devt;
    lf->inode.i_cdev = cdev;
    lf->file.f_op = fops;
    lf->file.f_inode = &lf->inode;
    lf->file.f_flags = (unsigned int)flags;
    lf->file.f_mode = FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
    if ((flags & O_ACCMODE) != O_WRONLY) lf->file.f_mode |= FMODE_READ;
    if ((flags & O_ACCMODE) != O_RDONLY) lf->file.f_mode |= FMODE_WRITE;
    mutex_init(&lf->file.f_pos_lock);
    lf->file.private_data = misc;               /* as misc_open does */
    if (fops->open) {
        int r = fops->open(&lf->inode, &lf->file);
        if (r) { kfree(lf); return r; }
    }
    *out = lf;
    return 0;
}

static void node_release(void *f)
{
    struct lkpi_file *lf = f;
    if (lf->file.f_op->release) lf->file.f_op->release(&lf->inode, &lf->file);
    kfree(lf);
}

static long node_rw(struct lkpi_file *lf, char *buf, unsigned long n, long long *pos, bool write)
{
    const struct file_operations *fops = lf->file.f_op;
    if (write ? !fops->write : !fops->read) return -EINVAL;
    struct task_struct *me = current;
    int saved = me->lkpi_uaccess_kernel;
    loff_t p = *pos;
    me->lkpi_uaccess_kernel = 1;
    ssize_t r = write ? fops->write(&lf->file, (const char __user *)buf, n, &p)
                      : fops->read(&lf->file, (char __user *)buf, n, &p);
    me->lkpi_uaccess_kernel = saved;
    if (r >= 0) { *pos = p; lf->file.f_pos = p; }
    return r;
}

static long node_read(void *f, char *buf, unsigned long n, long long *pos) { return node_rw(f, buf, n, pos, false); }
static long node_write(void *f, const char *buf, unsigned long n, long long *pos) { return node_rw(f, (char *)buf, n, pos, true); }

static long node_ioctl(void *f, unsigned int cmd, unsigned long arg)
{
    struct lkpi_file *lf = f;
    if (!lf->file.f_op->unlocked_ioctl) return -ENOTTY;
    long r = lf->file.f_op->unlocked_ioctl(&lf->file, cmd, arg);
    return r == -ENOIOCTLCMD ? -ENOTTY : r;
}

static int node_poll(void *f, int events)
{
    struct lkpi_file *lf = f;
    if (!lf->file.f_op->poll) return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
    poll_table pt = { ._key = (__poll_t)events };
    __poll_t mask = lf->file.f_op->poll(&lf->file, &pt);
    return (int)(mask & ((__poll_t)events | POLLERR | POLLHUP));
}

static const struct lkpi_cdev_ops node_ops = {
    .open = node_open, .release = node_release, .read = node_read, .write = node_write,
    .ioctl = node_ioctl, .poll = node_poll,
};

void *lkpi_devnode_add(struct device *dev)
{
    struct devnode *dn = kzalloc(sizeof *dn, GFP_KERNEL);
    umode_t mode = 0600;
    const char *name = dev_name(dev);
    char *tmp = NULL;
    if (!dn) return NULL;
    if (dev->class && dev->class->devnode) {
        tmp = dev->class->devnode(dev, &mode);
        if (tmp) name = tmp;
    }
    if (dev->class == &misc_class) {
        struct miscdevice *m = dev_get_drvdata(dev);
        if (m && m->mode) mode = m->mode;
    }
    dn->devt = dev->devt;
    dn->node = lkpi_cdev_add(name, mode ? mode : 0600, MAJOR(dev->devt), MINOR(dev->devt), &node_ops, dn);
    kfree(tmp);
    if (!dn->node) {
        printk(KERN_WARNING "linuxkpi: cannot create /dev/%s\n", dev_name(dev));
        kfree(dn);
        return NULL;
    }
    return dn;
}

/* the node stays in /dev but no longer opens; the devnode is kept for open files */
void lkpi_devnode_remove(struct device *dev)
{
    struct devnode *dn = dev->lkpi_devnode;
    dev->lkpi_devnode = NULL;
    if (dn) lkpi_cdev_remove(dn->node);
}

/* ---- file helpers ------------------------------------------------------------------------ */
loff_t noop_llseek(struct file *file, loff_t offset, int whence) { return file->f_pos; }
loff_t no_llseek(struct file *file, loff_t offset, int whence) { return -ESPIPE; }

loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size)
{
    loff_t pos;
    switch (whence) {
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = file->f_pos + offset; break;
    case SEEK_END: pos = size + offset; break;
    default: return -EINVAL;
    }
    if (pos < 0 || pos > size) return -EINVAL;
    file->f_pos = pos;
    return pos;
}

loff_t default_llseek(struct file *file, loff_t offset, int whence)
{
    loff_t pos;
    switch (whence) {
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = file->f_pos + offset; break;
    case SEEK_END: pos = file->f_inode->i_size + offset; break;
    default: return -EINVAL;
    }
    if (pos < 0) return -EINVAL;
    file->f_pos = pos;
    return pos;
}

int nonseekable_open(struct inode *inode, struct file *filp)
{
    filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
    return 0;
}

int stream_open(struct inode *inode, struct file *filp)
{
    filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
    filp->f_mode |= FMODE_STREAM;
    return 0;
}

int simple_open(struct inode *inode, struct file *file)
{
    if (inode->i_private) file->private_data = inode->i_private;
    return 0;
}

ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available)
{
    loff_t pos = *ppos;
    if (pos < 0) return -EINVAL;
    if ((size_t)pos >= available || !count) return 0;
    if (count > available - (size_t)pos) count = available - (size_t)pos;
    size_t left = copy_to_user(to, (const char *)from + pos, count);
    if (left == count) return -EFAULT;
    count -= left;
    *ppos = pos + (loff_t)count;
    return (ssize_t)count;
}

ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos, const void __user *from, size_t count)
{
    loff_t pos = *ppos;
    if (pos < 0) return -EINVAL;
    if ((size_t)pos >= available || !count) return 0;
    if (count > available - (size_t)pos) count = available - (size_t)pos;
    size_t left = copy_from_user((char *)to + pos, from, count);
    if (left == count) return -EFAULT;
    count -= left;
    *ppos = pos + (loff_t)count;
    return (ssize_t)count;
}
