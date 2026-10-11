/* include/kernel/inotify.h -- inotify(7) (fs/inotify.c) */
#ifndef KESTREL_INOTIFY_H
#define KESTREL_INOTIFY_H

#include <stdint.h>
#include <stdbool.h>

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_ALL_EVENTS    0x00000fff
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_CREATE   0x10000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000

struct vnode;
struct file;

/* reported by the VFS */
void     inotify_event(struct vnode *vn, uint32_t mask, const char *name, uint32_t cookie);
void     inotify_self(struct vnode *vn, uint32_t mask);    /* vn's watches + its directory's */
void     inotify_vnode_gone(struct vnode *vn);
uint32_t inotify_cookie(void);                             /* pairs IN_MOVED_FROM with IN_MOVED_TO */

/* the system calls */
int  inotify_create(int flags, struct file **out);
bool inotify_is(struct file *f);
int  inotify_add(struct file *f, struct vnode *vn, uint32_t mask);
int  inotify_rm(struct file *f, int wd);

#endif
