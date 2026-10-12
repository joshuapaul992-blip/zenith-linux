/* linuxkpi/src/workqueue.c -- workqueues
 *
 * A workqueue is a list of pending work items and a few kernel threads
 * (workers) that take them in order: one worker for an ordered queue, two
 * otherwise; the system queues are one pool of four. Bit 0 of work->data
 * is "pending": set when queued, cleared just before the function runs,
 * so the function may queue its own work again. A worker skips an item
 * whose previous run is still going on another worker, so a work item
 * never runs twice at once. The function may free its work item, so
 * workers remember it only by address.
 *
 * Delayed work is a timer that queues the work when it fires. */
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/printk.h>
#include "internal.h"

#define MAX_WORKERS     4

struct worker {
    struct workqueue_struct *wq;
    struct task_struct      *task;
    struct work_struct      *current_work;      /* by address only */
    unsigned long            done;              /* items finished  */
};

struct workqueue_struct {
    char                name[24];
    unsigned int        flags;
    spinlock_t          lock;
    struct list_head    pending;
    wait_queue_head_t   more_work;              /* workers wait here      */
    wait_queue_head_t   work_done;              /* flushers wait here     */
    int                 nworkers;
    struct worker       workers[MAX_WORKERS];
    bool                dying;
    struct list_head    node;
};

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_power_efficient_wq;
struct workqueue_struct *system_bh_wq;
struct workqueue_struct *system_bh_highpri_wq;

static LIST_HEAD(all_wqs);
static DEFINE_SPINLOCK(all_wqs_lock);

/* wq->lock held: is the item at w being run by a worker? */
static bool running_locked(struct workqueue_struct *wq, struct work_struct *w)
{
    for (int i = 0; i < wq->nworkers; i++)
        if (wq->workers[i].current_work == w) return true;
    return false;
}

static bool is_running(struct workqueue_struct *wq, struct work_struct *w)
{
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    bool r = running_locked(wq, w);
    spin_unlock_irqrestore(&wq->lock, flags);
    return r;
}

/* wq->lock held: the first pending item that is not running elsewhere */
static struct work_struct *runnable_locked(struct workqueue_struct *wq)
{
    struct work_struct *w;
    list_for_each_entry(w, &wq->pending, entry)
        if (!running_locked(wq, w)) return w;
    return NULL;
}

static bool has_runnable(struct workqueue_struct *wq)
{
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    bool r = runnable_locked(wq) != NULL;
    spin_unlock_irqrestore(&wq->lock, flags);
    return r;
}

static int worker_thread(void *arg)
{
    struct worker *me = arg;
    struct workqueue_struct *wq = me->wq;
    unsigned long flags;

    for (;;) {
        wait_event(wq->more_work, has_runnable(wq) || kthread_should_stop());
        spin_lock_irqsave(&wq->lock, flags);
        struct work_struct *w = runnable_locked(wq);
        if (!w) {
            spin_unlock_irqrestore(&wq->lock, flags);
            if (kthread_should_stop()) break;
            continue;
        }
        list_del_init(&w->entry);
        me->current_work = w;
        clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
        spin_unlock_irqrestore(&wq->lock, flags);

        w->func(w);

        spin_lock_irqsave(&wq->lock, flags);
        me->current_work = NULL;
        me->done++;
        spin_unlock_irqrestore(&wq->lock, flags);
        wake_up_all(&wq->work_done);
        wake_up_all(&wq->more_work);            /* an item may have waited for this one */
    }
    return 0;
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
    struct workqueue_struct *wq = kzalloc(sizeof *wq, GFP_KERNEL);
    if (!wq) return NULL;
    va_list ap;
    va_start(ap, max_active);
    vscnprintf(wq->name, sizeof wq->name, fmt, ap);
    va_end(ap);
    wq->flags = flags;
    spin_lock_init(&wq->lock);
    INIT_LIST_HEAD(&wq->pending);
    init_waitqueue_head(&wq->more_work);
    init_waitqueue_head(&wq->work_done);
    int n = (flags & __WQ_ORDERED) || max_active == 1 ? 1 : 2;
    for (int i = 0; i < n; i++) {
        struct worker *wk = &wq->workers[i];
        wk->wq = wq;
        wk->task = kthread_create(worker_thread, wk, "kworker/%s", wq->name);
        if (IS_ERR(wk->task)) {
            wk->task = NULL;
            if (!i) { kfree(wq); return NULL; }
            break;
        }
        wq->nworkers = i + 1;
        wake_up_process(wk->task);
    }
    unsigned long f;
    spin_lock_irqsave(&all_wqs_lock, f);
    list_add_tail(&wq->node, &all_wqs);
    spin_unlock_irqrestore(&all_wqs_lock, f);
    return wq;
}

static bool __queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
    unsigned long flags;
    if (!wq) wq = system_wq;
    spin_lock_irqsave(&wq->lock, flags);
    if (wq->dying) {
        spin_unlock_irqrestore(&wq->lock, flags);
        printk(KERN_WARNING "linuxkpi: work queued on %s, which is being destroyed\n", wq->name);
        clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
        return false;
    }
    w->lkpi_wq = wq;
    list_add_tail(&w->entry, &wq->pending);
    spin_unlock_irqrestore(&wq->lock, flags);
    wake_up(&wq->more_work);
    return true;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *w)
{
    if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))) return false;
    return __queue_work(wq, w);
}

void lkpi_delayed_work_timer_fn(struct timer_list *t)
{
    struct delayed_work *dw = from_timer(dw, t, timer);
    __queue_work(dw->wq, &dw->work);
}

/* the pending bit is ours: queue now or arm the timer */
static void __queue_delayed(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
    dw->wq = wq ? wq : system_wq;
    dw->work.lkpi_wq = dw->wq;
    if (!delay) { __queue_work(dw->wq, &dw->work); return; }
    dw->timer.function = lkpi_delayed_work_timer_fn;
    mod_timer(&dw->timer, jiffies + delay);
}

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
    if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dw->work))) return false;
    __queue_delayed(wq, dw, delay);
    return true;
}

/*
 * Take a work item off its timer or queue and own its pending bit:
 * 1 if it was pending, 0 if it was idle. When it is between the two (its
 * timer is firing) wait for that to finish.
 */
static int grab_pending(struct work_struct *w, struct delayed_work *dw)
{
    for (;;) {
        if (dw && timer_delete(&dw->timer)) return 1;
        if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))) return 0;
        struct workqueue_struct *wq = READ_ONCE(w->lkpi_wq);
        if (wq) {
            unsigned long flags;
            spin_lock_irqsave(&wq->lock, flags);
            if (!list_empty(&w->entry) && test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w))) {
                list_del_init(&w->entry);
                spin_unlock_irqrestore(&wq->lock, flags);
                return 1;
            }
            spin_unlock_irqrestore(&wq->lock, flags);
        }
        lkpi_task_yield();
    }
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dw, unsigned long delay)
{
    int was = grab_pending(&dw->work, dw);
    __queue_delayed(wq, dw, delay);
    return was;
}

static void wait_not_running(struct work_struct *w)
{
    struct workqueue_struct *wq = READ_ONCE(w->lkpi_wq);
    if (wq) wait_event(wq->work_done, !is_running(wq, w));
}

bool cancel_work(struct work_struct *w)
{
    int was = grab_pending(w, NULL);
    clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
    return was;
}

bool cancel_work_sync(struct work_struct *w)
{
    int was = grab_pending(w, NULL);
    wait_not_running(w);
    clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(w));
    return was;
}

bool cancel_delayed_work(struct delayed_work *dw)
{
    int was = grab_pending(&dw->work, dw);
    clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dw->work));
    return was;
}

bool cancel_delayed_work_sync(struct delayed_work *dw)
{
    int was = grab_pending(&dw->work, dw);
    wait_not_running(&dw->work);
    clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&dw->work));
    return was;
}

bool flush_work(struct work_struct *w)
{
    struct workqueue_struct *wq = READ_ONCE(w->lkpi_wq);
    if (!wq) return false;
    might_sleep();
    bool waited = work_pending(w) || is_running(wq, w);
    wait_event(wq->work_done, !work_pending(w) && !is_running(wq, w));
    return waited;
}

bool flush_delayed_work(struct delayed_work *dw)
{
    if (timer_delete_sync(&dw->timer)) __queue_work(dw->wq, &dw->work);
    return flush_work(&dw->work);
}

/*
 * flush_workqueue: queue a barrier item and wait for it; items queued
 * before it have then started, so wait as well for those still running
 * on other workers (each worker counts the items it finishes).
 */
struct wq_barrier {
    struct work_struct  work;
    struct completion   done;
    unsigned long       count[MAX_WORKERS];
    bool                busy[MAX_WORKERS];
};

static void barrier_fn(struct work_struct *w)
{
    struct wq_barrier *b = container_of(w, struct wq_barrier, work);
    struct workqueue_struct *wq = w->lkpi_wq;
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    for (int i = 0; i < wq->nworkers; i++) {
        b->busy[i] = wq->workers[i].current_work && wq->workers[i].current_work != w;
        b->count[i] = wq->workers[i].done;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    complete(&b->done);
}

static bool worker_moved_on(struct workqueue_struct *wq, int i, unsigned long count)
{
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    bool r = wq->workers[i].done != count;
    spin_unlock_irqrestore(&wq->lock, flags);
    return r;
}

void flush_workqueue(struct workqueue_struct *wq)
{
    struct wq_barrier b;
    might_sleep();
    INIT_WORK_ONSTACK(&b.work, barrier_fn);
    init_completion(&b.done);
    memset(b.busy, 0, sizeof b.busy);
    queue_work(wq, &b.work);
    wait_for_completion(&b.done);
    for (int i = 0; i < wq->nworkers; i++)
        if (b.busy[i]) wait_event(wq->work_done, worker_moved_on(wq, i, b.count[i]));
}

void drain_workqueue(struct workqueue_struct *wq)
{
    for (;;) {
        flush_workqueue(wq);
        unsigned long flags;
        spin_lock_irqsave(&wq->lock, flags);
        bool empty = list_empty(&wq->pending);
        spin_unlock_irqrestore(&wq->lock, flags);
        if (empty) break;
    }
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    unsigned long flags;
    if (!wq) return;
    drain_workqueue(wq);
    spin_lock_irqsave(&wq->lock, flags);
    wq->dying = true;
    spin_unlock_irqrestore(&wq->lock, flags);
    for (int i = 0; i < wq->nworkers; i++) kthread_stop(wq->workers[i].task);
    spin_lock_irqsave(&all_wqs_lock, flags);
    list_del(&wq->node);
    spin_unlock_irqrestore(&all_wqs_lock, flags);
    kfree(wq);
}

/* the worker running on this thread, if it is one */
static struct worker *current_worker(void)
{
    struct task_struct *me = current;
    struct workqueue_struct *wq;
    struct worker *found = NULL;
    unsigned long flags;
    spin_lock_irqsave(&all_wqs_lock, flags);
    list_for_each_entry(wq, &all_wqs, node)
        for (int i = 0; i < wq->nworkers; i++)
            if (wq->workers[i].task == me) found = &wq->workers[i];
    spin_unlock_irqrestore(&all_wqs_lock, flags);
    return found;
}

struct work_struct *current_work(void)
{
    struct worker *w = current_worker();
    return w ? w->current_work : NULL;
}

bool current_is_workqueue_rescuer(void) { return false; }

static void rcu_work_rcufn(struct rcu_head *rcu)
{
    struct rcu_work *rw = container_of(rcu, struct rcu_work, rcu);
    __queue_work(rw->wq, &rw->work);
}

bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw)
{
    if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&rw->work))) return false;
    rw->wq = wq;
    rw->work.lkpi_wq = wq;
    call_rcu(&rw->rcu, rcu_work_rcufn);
    return true;
}

bool flush_rcu_work(struct rcu_work *rw)
{
    if (work_pending(&rw->work)) rcu_barrier();
    return flush_work(&rw->work);
}

void lkpi_workqueue_core_init(void)
{
    struct workqueue_struct *wq = alloc_workqueue("events", 0, 0);
    if (!wq) lkpi_die("linuxkpi: cannot start the system workqueue");
    /* the system pool: two more workers than a private queue */
    for (int i = wq->nworkers; i < MAX_WORKERS; i++) {
        struct worker *wk = &wq->workers[i];
        wk->wq = wq;
        wk->task = kthread_create(worker_thread, wk, "kworker/events");
        if (IS_ERR(wk->task)) { wk->task = NULL; break; }
        wq->nworkers = i + 1;
        wake_up_process(wk->task);
    }
    system_wq = system_highpri_wq = system_long_wq = system_unbound_wq = wq;
    system_freezable_wq = system_power_efficient_wq = system_freezable_power_efficient_wq = wq;
    system_bh_wq = system_bh_highpri_wq = wq;
}
