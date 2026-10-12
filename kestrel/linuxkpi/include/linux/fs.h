/* linux/fs.h -- Kestrel's Linux KPI: files of character devices (linuxkpi/src/chardev.c)
 *
 * Only what device drivers use: struct file and struct inode as the
 * driver's file_operations see them, character device numbers and a few
 * helpers. The files themselves are Kestrel's (devfs). */
#ifndef _LKPI_LINUX_FS_H
#define _LKPI_LINUX_FS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/kdev_t.h>
#include <linux/stat.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/kref.h>

struct module;
struct vm_area_struct;
struct cdev;

#define FMODE_READ              ((fmode_t)0x1)
#define FMODE_WRITE             ((fmode_t)0x2)
#define FMODE_LSEEK             ((fmode_t)0x4)
#define FMODE_PREAD             ((fmode_t)0x8)
#define FMODE_PWRITE            ((fmode_t)0x10)
#define FMODE_EXEC              ((fmode_t)0x20)
#define FMODE_STREAM            ((fmode_t)0x200000)

#define O_ACCMODE       00000003
#define O_RDONLY        00000000
#define O_WRONLY        00000001
#define O_RDWR          00000002
#define O_CREAT         00000100
#define O_EXCL          00000200
#define O_NOCTTY        00000400
#define O_TRUNC         00001000
#define O_APPEND        00002000
#define O_NONBLOCK      00004000
#define O_NDELAY        O_NONBLOCK
#define O_CLOEXEC       02000000

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

struct inode {
    umode_t             i_mode;
    dev_t               i_rdev;
    loff_t              i_size;
    struct cdev        *i_cdev;
    void               *i_private;
};

struct file_operations;
struct file {
    const struct file_operations *f_op;
    struct inode       *f_inode;
    unsigned int        f_flags;
    fmode_t             f_mode;
    loff_t              f_pos;
    void               *private_data;
    struct mutex        f_pos_lock;
};

struct file_operations {
    struct module *owner;
    loff_t   (*llseek)(struct file *, loff_t, int);
    ssize_t  (*read)(struct file *, char __user *, size_t, loff_t *);
    ssize_t  (*write)(struct file *, const char __user *, size_t, loff_t *);
    __poll_t (*poll)(struct file *, struct poll_table_struct *);
    long     (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
    long     (*compat_ioctl)(struct file *, unsigned int, unsigned long);
    int      (*mmap)(struct file *, struct vm_area_struct *);
    int      (*open)(struct inode *, struct file *);
    int      (*flush)(struct file *, void *id);
    int      (*release)(struct inode *, struct file *);
    int      (*fsync)(struct file *, loff_t, loff_t, int datasync);
    int      (*fasync)(int, struct file *, int);
};

static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
static inline unsigned iminor(const struct inode *inode) { return MINOR(inode->i_rdev); }
static inline unsigned imajor(const struct inode *inode) { return MAJOR(inode->i_rdev); }
loff_t noop_llseek(struct file *file, loff_t offset, int whence);
loff_t no_llseek(struct file *file, loff_t offset, int whence);
loff_t default_llseek(struct file *file, loff_t offset, int whence);
loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size);
int nonseekable_open(struct inode *inode, struct file *filp);
int stream_open(struct inode *inode, struct file *filp);
int simple_open(struct inode *inode, struct file *file);
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available);
ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos, const void __user *from, size_t count);
#define compat_ptr_ioctl        NULL

/* character device numbers: register_chrdev() makes /dev/<name><minor> nodes on cdev_add */
int  register_chrdev_region(dev_t from, unsigned count, const char *name);
int  alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name);
void unregister_chrdev_region(dev_t from, unsigned count);
int  __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name,
                       const struct file_operations *fops);
void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name);
static inline int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops)
{ return __register_chrdev(major, 0, 256, name, fops); }
static inline void unregister_chrdev(unsigned int major, const char *name) { __unregister_chrdev(major, 0, 256, name); }

#endif
