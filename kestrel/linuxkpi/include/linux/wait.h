/* linux/wait.h -- Kestrel's Linux KPI: wait queues (linuxkpi/src/sched.c)
 *
 * A wait queue is a spinlock and a list of waiters, each a thread and a
 * wake function. wait_event*() add the caller, set its sleep state, test
 * the condition and sleep, until the condition holds (or a timeout or
 * signal); wake_up*() run the wake functions of the waiters, stopping
 * after nr exclusive ones. */
#ifndef _LKPI_LINUX_WAIT_H
#define _LKPI_LINUX_WAIT_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);

#define WQ_FLAG_EXCLUSIVE       0x01
#define WQ_FLAG_WOKEN           0x02

struct wait_queue_entry {
    unsigned int        flags;
    void               *private;            /* the waiting task */
    wait_queue_func_t   func;
    struct list_head    entry;
};
typedef struct wait_queue_entry wait_queue_entry_t;

struct wait_queue_head {
    spinlock_t          lock;
    struct list_head    head;
};
typedef struct wait_queue_head wait_queue_head_t;

int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { .lock = __SPIN_LOCK_UNLOCKED(name.lock), .head = LIST_HEAD_INIT(name.head) }
#define DECLARE_WAIT_QUEUE_HEAD(name)   struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
#define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name) DECLARE_WAIT_QUEUE_HEAD(name)
#define __WAITQUEUE_INITIALIZER(name, tsk) { .private = tsk, .func = default_wake_function, .entry = { NULL, NULL } }
#define DECLARE_WAITQUEUE(name, tsk)    struct wait_queue_entry name = __WAITQUEUE_INITIALIZER(name, tsk)
#define DEFINE_WAIT_FUNC(name, function) \
    struct wait_queue_entry name = { .private = current, .func = function, .entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT(name)               DEFINE_WAIT_FUNC(name, autoremove_wake_function)

static inline void init_waitqueue_head(struct wait_queue_head *wq) { spin_lock_init(&wq->lock); INIT_LIST_HEAD(&wq->head); }
#define __init_waitqueue_head(wq, name, key) init_waitqueue_head(wq)
static inline void init_waitqueue_entry(struct wait_queue_entry *e, struct task_struct *p)
{ e->flags = 0; e->private = p; e->func = default_wake_function; }
static inline void init_waitqueue_func_entry(struct wait_queue_entry *e, wait_queue_func_t func)
{ e->flags = 0; e->private = NULL; e->func = func; }
static inline void init_wait_entry_kpi(struct wait_queue_entry *e, int flags)
{ e->flags = flags; e->private = current; e->func = autoremove_wake_function; INIT_LIST_HEAD(&e->entry); }
#define init_wait(wait)         init_wait_entry_kpi(wait, 0)
#define init_wait_entry(w, f)   init_wait_entry_kpi(w, f)

static inline int waitqueue_active(struct wait_queue_head *wq) { return !list_empty(&wq->head); }
static inline bool wq_has_sleeper(struct wait_queue_head *wq) { smp_mb(); return waitqueue_active(wq); }

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e);
void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
bool prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
/* prepare_to_wait() for the wait_event macros: -ERESTARTSYS when a signal ends an interruptible wait */
long prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
long wait_woken(struct wait_queue_entry *e, unsigned mode, long timeout);

void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key);
void __wake_up_locked(struct wait_queue_head *wq, unsigned int mode, int nr);
#define wake_up(x)                      __wake_up(x, TASK_NORMAL, 1, NULL)
#define wake_up_nr(x, nr)               __wake_up(x, TASK_NORMAL, nr, NULL)
#define wake_up_all(x)                  __wake_up(x, TASK_NORMAL, 0, NULL)
#define wake_up_locked(x)               __wake_up_locked((x), TASK_NORMAL, 1)
#define wake_up_all_locked(x)           __wake_up_locked((x), TASK_NORMAL, 0)
#define wake_up_interruptible(x)        __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_interruptible_nr(x, nr) __wake_up(x, TASK_INTERRUPTIBLE, nr, NULL)
#define wake_up_interruptible_all(x)    __wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
#define wake_up_interruptible_sync(x)   __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_poll(x, m)              __wake_up(x, TASK_NORMAL, 1, (void *)(uintptr_t)(m))
#define wake_up_interruptible_poll(x, m) __wake_up(x, TASK_INTERRUPTIBLE, 1, (void *)(uintptr_t)(m))

/*
 * The one loop behind every wait_event variant. `state` is the sleep
 * state, `ret` the initial value of the result (the timeout for timed
 * waits), `sleep` the statement that sleeps and may update __ret.
 * Ends with __ret: for an interruptible wait -ERESTARTSYS on a signal.
 */
#define __lkpi_wait_event(wq_head, condition, state, exclusive, ret, sleep) ({ \
        struct wait_queue_entry __we; \
        long __ret = ret; \
        init_wait_entry(&__we, (exclusive) ? WQ_FLAG_EXCLUSIVE : 0); \
        for (;;) { \
            long __sig = prepare_to_wait_event(&(wq_head), &__we, state); \
            if (condition) break; \
            if (__sig) { __ret = __sig; break; } \
            sleep; \
        } \
        finish_wait(&(wq_head), &__we); \
        __ret; })

/* timed waits: keep the remaining time in __ret; when it reaches 0 the
 * condition is tested once more (true gives 1, like Linux) */
#define __lkpi_wait_event_timeout(wq_head, condition, state, timeout) ({ \
        long __r = __lkpi_wait_event(wq_head, condition, state, 0, timeout, \
            ({ __ret = schedule_timeout(__ret); if (!__ret) { __ret = (condition) ? 1 : 0; break; } })); \
        __r; })

#define wait_event(wq, condition) do { \
        might_sleep(); \
        if (condition) break; \
        (void)__lkpi_wait_event(wq, condition, TASK_UNINTERRUPTIBLE, 0, 0, schedule()); } while (0)
#define wait_event_idle(wq, condition) wait_event(wq, condition)
#define wait_event_freezable(wq, condition) wait_event_interruptible(wq, condition)
#define wait_event_interruptible(wq, condition) ({ \
        long __res = 0; might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event(wq, condition, TASK_INTERRUPTIBLE, 0, 0, schedule()); \
        (int)__res; })
#define wait_event_killable(wq, condition) ({ \
        long __res = 0; might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event(wq, condition, TASK_KILLABLE, 0, 0, schedule()); \
        (int)__res; })
#define wait_event_interruptible_exclusive(wq, condition) ({ \
        long __res = 0; might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event(wq, condition, TASK_INTERRUPTIBLE, 1, 0, schedule()); \
        (int)__res; })
#define wait_event_exclusive_kpi(wq, condition) do { \
        if (condition) break; \
        (void)__lkpi_wait_event(wq, condition, TASK_UNINTERRUPTIBLE, 1, 0, schedule()); } while (0)
/* 0 when the time ran out with the condition false, else the jiffies left (>= 1) */
#define wait_event_timeout(wq, condition, timeout) ({ \
        long __res = (timeout); might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event_timeout(wq, condition, TASK_UNINTERRUPTIBLE, timeout); \
        else if (!__res) __res = 1; \
        __res; })
#define wait_event_idle_timeout(wq, condition, timeout) wait_event_timeout(wq, condition, timeout)
#define wait_event_freezable_timeout(wq, condition, timeout) wait_event_interruptible_timeout(wq, condition, timeout)
#define wait_event_interruptible_timeout(wq, condition, timeout) ({ \
        long __res = (timeout); might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event_timeout(wq, condition, TASK_INTERRUPTIBLE, timeout); \
        else if (!__res) __res = 1; \
        __res; })
#define wait_event_killable_timeout(wq, condition, timeout) ({ \
        long __res = (timeout); might_sleep(); \
        if (!(condition)) __res = __lkpi_wait_event_timeout(wq, condition, TASK_KILLABLE, timeout); \
        else if (!__res) __res = 1; \
        __res; })
/* the caller holds wq.lock (spin_lock_irq); it is dropped while sleeping */
#define wait_event_lock_irq(wq, condition, lock) do { \
        if (condition) break; \
        (void)__lkpi_wait_event(wq, condition, TASK_UNINTERRUPTIBLE, 0, 0, \
                                ({ spin_unlock_irq(&(lock)); schedule(); spin_lock_irq(&(lock)); })); } while (0)
#define wait_event_interruptible_lock_irq(wq, condition, lock) ({ \
        long __res = 0; \
        if (!(condition)) __res = __lkpi_wait_event(wq, condition, TASK_INTERRUPTIBLE, 0, 0, \
                                ({ spin_unlock_irq(&(lock)); schedule(); spin_lock_irq(&(lock)); })); \
        (int)__res; })
#endif
