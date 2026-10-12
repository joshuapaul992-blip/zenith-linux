/* linux/semaphore.h -- Kestrel's Linux KPI: counting semaphores */
#ifndef _LKPI_LINUX_SEMAPHORE_H
#define _LKPI_LINUX_SEMAPHORE_H

#include <linux/wait.h>

struct semaphore {
    raw_spinlock_t      lock;
    unsigned int        count;
    wait_queue_head_t   wait;
};

#define __SEMAPHORE_INITIALIZER(name, n) \
    { .lock = __RAW_SPIN_LOCK_UNLOCKED(name.lock), .count = n, .wait = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait) }
#define DEFINE_SEMAPHORE(name, n)       struct semaphore name = __SEMAPHORE_INITIALIZER(name, n)
static inline void sema_init(struct semaphore *s, int val)
{ raw_spin_lock_init(&s->lock); s->count = val; init_waitqueue_head(&s->wait); }

void down(struct semaphore *s);
int  __must_check down_interruptible(struct semaphore *s);
int  __must_check down_killable(struct semaphore *s);
int  __must_check down_trylock(struct semaphore *s);     /* 0 if taken (sic) */
int  __must_check down_timeout(struct semaphore *s, long timeout);
void up(struct semaphore *s);

#endif
