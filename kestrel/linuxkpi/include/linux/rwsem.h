/* linux/rwsem.h -- Kestrel's Linux KPI: sleeping reader/writer locks (simple, no fairness) */
#ifndef _LKPI_LINUX_RWSEM_H
#define _LKPI_LINUX_RWSEM_H

#include <linux/wait.h>
#include <linux/atomic.h>

struct rw_semaphore {
    atomic_long_t       count;          /* readers, or -1 for a writer */
    wait_queue_head_t   wait;
};

#define __RWSEM_INITIALIZER(name)       { .count = ATOMIC_LONG_INIT(0), .wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait) }
#define DECLARE_RWSEM(name)             struct rw_semaphore name = __RWSEM_INITIALIZER(name)
static inline void init_rwsem(struct rw_semaphore *s) { atomic_long_set(&s->count, 0); init_waitqueue_head(&s->wait); }
static inline int rwsem_is_locked(struct rw_semaphore *s) { return atomic_long_read(&s->count) != 0; }

void down_read(struct rw_semaphore *s);
int  down_read_trylock(struct rw_semaphore *s);
int  down_read_interruptible(struct rw_semaphore *s);
int  down_read_killable(struct rw_semaphore *s);
void up_read(struct rw_semaphore *s);
void down_write(struct rw_semaphore *s);
int  down_write_trylock(struct rw_semaphore *s);
int  down_write_killable(struct rw_semaphore *s);
void up_write(struct rw_semaphore *s);
void downgrade_write(struct rw_semaphore *s);
#define down_read_nested(s, sub)        down_read(s)
#define down_write_nested(s, sub)       down_write(s)

#endif
