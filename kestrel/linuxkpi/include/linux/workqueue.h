/* linux/workqueue.h -- Kestrel's Linux KPI: deferred work (linuxkpi/src/workqueue.c)
 *
 * A workqueue is a list of pending work items served by its own kernel
 * threads (one for an ordered queue, a few otherwise). A work item is
 * never run by two workers at once. The system queues share one pool. */
#ifndef _LKPI_LINUX_WORKQUEUE_H
#define _LKPI_LINUX_WORKQUEUE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/bitops.h>

struct work_struct;
struct workqueue_struct;
typedef void (*work_func_t)(struct work_struct *work);

#define WORK_STRUCT_PENDING_BIT 0

struct work_struct {
    atomic_long_t               data;           /* bit 0: pending */
    struct list_head            entry;
    work_func_t                 func;
    struct workqueue_struct    *lkpi_wq;        /* where it was last queued */
};

struct delayed_work {
    struct work_struct          work;
    struct timer_list           timer;
    struct workqueue_struct    *wq;
    int                         cpu;
};

struct rcu_work {
    struct work_struct          work;
    struct rcu_head             rcu;
    struct workqueue_struct    *wq;
};

void lkpi_delayed_work_timer_fn(struct timer_list *t);

#define __WORK_INITIALIZER(n, f)        { .data = ATOMIC_LONG_INIT(0), .entry = { &(n).entry, &(n).entry }, .func = (f) }
#define __DELAYED_WORK_INITIALIZER(n, f, tflags) \
    { .work = __WORK_INITIALIZER((n).work, (f)), \
      .timer = { .entry = { NULL, NULL }, .function = lkpi_delayed_work_timer_fn, .flags = (tflags) } }
#define DECLARE_WORK(n, f)              struct work_struct n = __WORK_INITIALIZER(n, f)
#define DECLARE_DELAYED_WORK(n, f)      struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, 0)
#define DECLARE_DEFERRABLE_WORK(n, f)   struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, TIMER_DEFERRABLE)

static inline void lkpi_init_work(struct work_struct *w, work_func_t f)
{
    atomic_long_set(&w->data, 0);
    INIT_LIST_HEAD(&w->entry);
    w->func = f;
    w->lkpi_wq = NULL;
}
#define INIT_WORK(w, f)                 lkpi_init_work((w), (f))
#define INIT_WORK_ONSTACK(w, f)         lkpi_init_work((w), (f))
#define __INIT_WORK(w, f, onstack)      lkpi_init_work((w), (f))
#define destroy_work_on_stack(w)        do { } while (0)
#define destroy_delayed_work_on_stack(w) do { } while (0)
#define INIT_DELAYED_WORK(dw, f) do { \
        lkpi_init_work(&(dw)->work, (f)); \
        timer_setup(&(dw)->timer, lkpi_delayed_work_timer_fn, 0); \
        (dw)->wq = NULL; (dw)->cpu = 0; } while (0)
#define INIT_DELAYED_WORK_ONSTACK(dw, f) INIT_DELAYED_WORK(dw, f)
#define INIT_DEFERRABLE_WORK(dw, f)     INIT_DELAYED_WORK(dw, f)
#define INIT_RCU_WORK(rw, f)            INIT_WORK(&(rw)->work, (f))

static inline struct delayed_work *to_delayed_work(struct work_struct *w) { return container_of(w, struct delayed_work, work); }
static inline struct rcu_work *to_rcu_work(struct work_struct *w) { return container_of(w, struct rcu_work, work); }
#define work_data_bits(w)               ((unsigned long *)(&(w)->data))
#define work_pending(w)                 test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))
#define delayed_work_pending(dw)        work_pending(&(dw)->work)

enum {
    WQ_UNBOUND          = 1 << 1,
    WQ_FREEZABLE        = 1 << 2,
    WQ_MEM_RECLAIM      = 1 << 3,
    WQ_HIGHPRI          = 1 << 4,
    WQ_CPU_INTENSIVE    = 1 << 5,
    WQ_SYSFS            = 1 << 6,
    WQ_POWER_EFFICIENT  = 1 << 7,
    __WQ_ORDERED        = 1 << 17,
    WQ_MAX_ACTIVE       = 512,
    WQ_DFL_ACTIVE       = WQ_MAX_ACTIVE / 2,
};

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_freezable_wq;
extern struct workqueue_struct *system_power_efficient_wq;
extern struct workqueue_struct *system_freezable_power_efficient_wq;
extern struct workqueue_struct *system_bh_wq;
extern struct workqueue_struct *system_bh_highpri_wq;

__printf(1, 4) struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, args...) \
    alloc_workqueue(fmt, WQ_UNBOUND | __WQ_ORDERED | (flags), 1, ##args)
#define create_workqueue(name)          alloc_workqueue("%s", WQ_MEM_RECLAIM, 1, (name))
#define create_freezable_workqueue(name) alloc_workqueue("%s", WQ_FREEZABLE | WQ_UNBOUND | WQ_MEM_RECLAIM, 1, (name))
#define create_singlethread_workqueue(name) alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM, name)
void destroy_workqueue(struct workqueue_struct *wq);

/* true if queued, false if it was already pending */
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork);
#define queue_work_on(cpu, wq, w)               queue_work(wq, w)
#define queue_delayed_work_on(cpu, wq, dw, d)   queue_delayed_work(wq, dw, d)
#define mod_delayed_work_on(cpu, wq, dw, d)     mod_delayed_work(wq, dw, d)
static inline bool schedule_work(struct work_struct *w) { return queue_work(system_wq, w); }
static inline bool schedule_delayed_work(struct delayed_work *dw, unsigned long delay)
{ return queue_delayed_work(system_wq, dw, delay); }
#define schedule_work_on(cpu, w)                schedule_work(w)
#define schedule_delayed_work_on(cpu, dw, d)    schedule_delayed_work(dw, d)

/* wait until the last queueing of the work has run: true if it had to wait */
bool flush_work(struct work_struct *work);
bool flush_delayed_work(struct delayed_work *dwork);
bool flush_rcu_work(struct rcu_work *rwork);
void flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);
#define flush_scheduled_work()          flush_workqueue(system_wq)
/* take it off its queue (and timer); the _sync forms also wait for it to finish running */
bool cancel_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
#define disable_work_sync(w)            cancel_work_sync(w)
#define disable_delayed_work_sync(dw)   cancel_delayed_work_sync(dw)
struct work_struct *current_work(void);
bool current_is_workqueue_rescuer(void);
#define workqueue_set_max_active(wq, n) do { } while (0)

#endif
