/* linux/mutex.h -- Kestrel's Linux KPI: sleeping locks (linuxkpi/src/sched.c) */
#ifndef _LKPI_LINUX_MUTEX_H
#define _LKPI_LINUX_MUTEX_H

#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/lockdep.h>

struct mutex {
    struct task_struct *owner;          /* NULL: free */
    wait_queue_head_t   wait;
};

#define __MUTEX_INITIALIZER(name)       { .owner = NULL, .wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait) }
#define DEFINE_MUTEX(name)              struct mutex name = __MUTEX_INITIALIZER(name)

static inline void mutex_init(struct mutex *m) { m->owner = NULL; init_waitqueue_head(&m->wait); }
#define __mutex_init(m, name, key)      mutex_init(m)
static inline void mutex_destroy(struct mutex *m) { }
static inline bool mutex_is_locked(struct mutex *m) { return READ_ONCE(m->owner) != NULL; }

void mutex_lock(struct mutex *m);
int  __must_check mutex_lock_interruptible(struct mutex *m);
int  __must_check mutex_lock_killable(struct mutex *m);
int  mutex_trylock(struct mutex *m);            /* 1 if taken */
void mutex_unlock(struct mutex *m);
#define mutex_lock_nested(m, sub)       mutex_lock(m)
#define mutex_lock_io(m)                mutex_lock(m)
#define mutex_lock_interruptible_nested(m, sub) mutex_lock_interruptible(m)
#define mutex_lock_killable_nested(m, sub)      mutex_lock_killable(m)
#define devm_mutex_init(dev, m)         ({ mutex_init(m); 0; })
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock);

#endif
