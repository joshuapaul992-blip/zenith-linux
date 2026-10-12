/* linuxkpi/glue/devfs.c -- character devices of Linux drivers in /dev
 *
 * A node registered here looks like any other device in /dev. Each open
 * gets a vnode of its own, outside the tree (as /dev/ptmx does), so the
 * Linux side can keep per-open state (its struct file) in it; the open
 * file's last close calls release and frees it. */
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include "../include/lkpi/glue.h"

struct cdev_node {
    const struct lkpi_cdev_ops *ops;
    void         *dev;
    struct vnode *vn;               /* the node in /dev */
};

struct cdev_open {
    struct vnode      vn;           /* first: the open file's node */
    struct cdev_node *node;
    void             *file;         /* the Linux side's per-open state */
};

static struct cdev_open *of(struct file *f) { return (struct cdev_open *)f->vn; }

static ssize_t cdev_fread(struct file *f, void *buf, size_t len)
{
    struct cdev_open *o = of(f);
    if (!o->node->ops->read) return -EINVAL;
    long long pos = (long long)f->off;
    long r = o->node->ops->read(o->file, buf, len, &pos);
    if (r >= 0) f->off = (uint64_t)pos;
    return r;
}

static ssize_t cdev_fwrite(struct file *f, const void *buf, size_t len)
{
    struct cdev_open *o = of(f);
    if (!o->node->ops->write) return -EINVAL;
    long long pos = (long long)f->off;
    long r = o->node->ops->write(o->file, buf, len, &pos);
    if (r >= 0) f->off = (uint64_t)pos;
    return r;
}

static int cdev_fioctl(struct file *f, unsigned long req, void *arg)
{
    struct cdev_open *o = of(f);
    if (!o->node->ops->ioctl) return -ENOTTY;
    return (int)o->node->ops->ioctl(o->file, (unsigned int)req, (unsigned long)arg);
}

static int cdev_poll(struct file *f, int events)
{
    struct cdev_open *o = of(f);
    if (!o->node->ops->poll) return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
    return o->node->ops->poll(o->file, events);
}

static void cdev_release(struct file *f)
{
    struct cdev_open *o = of(f);
    if (o->node->ops->release) o->node->ops->release(o->file);
    kfree(o);
}

static const struct vnode_ops open_ops = {
    .fread = cdev_fread, .fwrite = cdev_fwrite, .fioctl = cdev_fioctl,
    .poll = cdev_poll, .release = cdev_release,
};

static int cdev_open(struct vnode *vn, int flags, struct file **out)
{
    struct cdev_node *n = vn->ctx;
    struct cdev_open *o = kzalloc(sizeof *o);
    if (!o) return -ENOMEM;
    memcpy(&o->vn, vn, sizeof o->vn);           /* name, mode, rdev, owner, fs */
    o->vn.ops = &open_ops;
    o->vn.parent = o->vn.children = o->vn.sibling = o->vn.mounted = NULL;
    o->vn.refs = 0;
    o->node = n;
    if (n->ops->open) {
        int r = n->ops->open(n->dev, flags, &o->file);
        if (r < 0) { kfree(o); return r; }
    }
    struct file *f = vfs_file_new(&o->vn, flags);
    if (!f) {
        if (n->ops->release) n->ops->release(o->file);
        kfree(o);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

static const struct vnode_ops node_ops = { .open = cdev_open };

void *lkpi_cdev_add(const char *name, unsigned int mode, unsigned int major, unsigned int minor,
                    const struct lkpi_cdev_ops *ops, void *dev)
{
    struct cdev_node *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    n->ops = ops;
    n->dev = dev;
    n->vn = devfs_register(name, S_IFCHR | (mode & 07777), (major << 8) | (minor & 0xFF), &node_ops, n);
    if (!n->vn) { kfree(n); return NULL; }
    return n;
}

/* The node stays in /dev (devfs has no removal yet) but opens fail; files
 * already open keep their callbacks, which the Linux side keeps valid. */
static int gone_open(struct vnode *vn, int flags, struct file **out) { return -ENODEV; }
static const struct vnode_ops gone_ops = { .open = gone_open };

void lkpi_cdev_remove(void *node)
{
    struct cdev_node *n = node;
    if (n && n->vn) n->vn->ops = &gone_ops;
}
