/* linuxkpi/src/timer.c -- jiffies, sleeps and timer_list
 *
 * jiffies count milliseconds since boot (HZ is 1000). Pending timers are
 * kept on one list in expiry order; the "lkpi-timer" thread sleeps until
 * the first one is due and runs the callbacks one at a time, outside the
 * lock. timer_delete_sync() waits for a callback that is running. */
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/printk.h>
#include "internal.h"

unsigned long lkpi_jiffies(void) { return (unsigned long)lkpi_time_ms(); }
ktime_t ktime_get(void) { return (ktime_t)lkpi_time_ns(); }
ktime_t ktime_get_real(void) { return (ktime_t)lkpi_realtime_ns(); }

/* ---- sleeps ------------------------------------------------------------------- */
void msleep(unsigned int msecs)
{
    long timeout = (long)msecs_to_jiffies(msecs) + 1;      /* at least msecs, whatever the phase */
    while (timeout) timeout = schedule_timeout_uninterruptible(timeout);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    long timeout = (long)msecs_to_jiffies(msecs) + 1;
    while (timeout && !signal_pending(current)) timeout = schedule_timeout_interruptible(timeout);
    return jiffies_to_msecs((unsigned long)timeout);
}

/* Short waits spin (a sleep cannot be shorter than a millisecond here);
 * longer ones sleep for at least min microseconds. */
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
    if (min <= 100) { lkpi_udelay((unsigned int)min); return; }
    unsigned long long until = lkpi_time_ns() + (unsigned long long)min * 1000;
    for (;;) {
        unsigned long long now = lkpi_time_ns();
        if (now >= until) break;
        long t = (long)nsecs_to_jiffies(until - now) + 1;
        __set_current_state(state);
        schedule_timeout(t);
        if ((state & TASK_INTERRUPTIBLE) && signal_pending(current)) break;
    }
    __set_current_state(TASK_RUNNING);
}

/* ---- timer_list ------------------------------------------------------------------- */
static LIST_HEAD(pending);
static DEFINE_SPINLOCK(timer_lock);
static struct timer_list *running;              /* the callback being run */
static DECLARE_WAIT_QUEUE_HEAD(running_done);
static struct task_struct *timer_task;

void lkpi_timer_init(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags)
{
    t->entry.next = t->entry.prev = NULL;
    t->expires = 0;
    t->function = fn;
    t->flags = flags;
}

/* timer_lock held: insert in expiry order; true if it is now the first */
static bool enqueue(struct timer_list *t)
{
    struct timer_list *pos;
    list_for_each_entry(pos, &pending, entry) {
        if (time_before(t->expires, pos->expires)) {
            list_add_tail(&t->entry, &pos->entry);
            return pending.next == &t->entry;
        }
    }
    list_add_tail(&t->entry, &pending);
    return pending.next == &t->entry;
}

static void dequeue(struct timer_list *t)
{
    list_del(&t->entry);
    t->entry.next = t->entry.prev = NULL;
}

static int modify(struct timer_list *t, unsigned long expires, bool only_pending, bool only_earlier)
{
    unsigned long flags;
    int was;
    bool first = false;

    spin_lock_irqsave(&timer_lock, flags);
    was = timer_pending(t);
    if ((only_pending && !was) || !t->function) {   /* a shut-down timer stays off */
        spin_unlock_irqrestore(&timer_lock, flags);
        return was;
    }
    if (was && only_earlier && !time_before(expires, t->expires)) {
        spin_unlock_irqrestore(&timer_lock, flags);
        return 1;
    }
    if (was) dequeue(t);
    t->expires = expires;
    first = enqueue(t);
    spin_unlock_irqrestore(&timer_lock, flags);
    if (first && timer_task) wake_up_process(timer_task);
    return was;
}

int mod_timer(struct timer_list *t, unsigned long expires) { return modify(t, expires, false, false); }
int mod_timer_pending(struct timer_list *t, unsigned long expires) { return modify(t, expires, true, false); }
int timer_reduce(struct timer_list *t, unsigned long expires) { return modify(t, expires, false, true); }
void add_timer(struct timer_list *t) { modify(t, t->expires, false, false); }

int timer_delete(struct timer_list *t)
{
    unsigned long flags;
    int was;
    spin_lock_irqsave(&timer_lock, flags);
    was = timer_pending(t);
    if (was) dequeue(t);
    spin_unlock_irqrestore(&timer_lock, flags);
    return was;
}

int timer_delete_sync(struct timer_list *t)
{
    int ret = 0;
    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&timer_lock, flags);
        if (timer_pending(t)) { dequeue(t); ret = 1; }
        bool busy = running == t;
        spin_unlock_irqrestore(&timer_lock, flags);
        if (!busy) return ret;
        if (current == timer_task) {
            printk(KERN_ERR "linuxkpi: timer_delete_sync from its own callback\n");
            return ret;
        }
        if (in_atomic()) cpu_relax();
        else wait_event(running_done, READ_ONCE(running) != t);
    }
}

int timer_shutdown_sync(struct timer_list *t)
{
    int ret = timer_delete_sync(t);
    t->function = NULL;
    return ret;
}

int timer_shutdown(struct timer_list *t)
{
    int ret = timer_delete(t);
    t->function = NULL;
    return ret;
}

static int timer_thread(void *unused)
{
    for (;;) {
        unsigned long flags;
        long timeout = MAX_SCHEDULE_TIMEOUT;

        set_current_state(TASK_INTERRUPTIBLE);
        spin_lock_irqsave(&timer_lock, flags);
        if (!list_empty(&pending)) {
            struct timer_list *t = list_first_entry(&pending, struct timer_list, entry);
            unsigned long now = jiffies;
            if (time_after_eq(now, t->expires)) {
                void (*fn)(struct timer_list *) = t->function;
                dequeue(t);
                running = t;
                spin_unlock_irqrestore(&timer_lock, flags);
                __set_current_state(TASK_RUNNING);
                if (fn) fn(t);
                spin_lock_irqsave(&timer_lock, flags);
                running = NULL;
                spin_unlock_irqrestore(&timer_lock, flags);
                wake_up_all(&running_done);
                continue;
            }
            timeout = (long)(t->expires - now);
        }
        spin_unlock_irqrestore(&timer_lock, flags);
        schedule_timeout(timeout);
    }
    return 0;
}

void lkpi_timer_core_init(void)
{
    struct task_struct *t = kthread_run(timer_thread, NULL, "lkpi-timer");
    if (IS_ERR(t)) lkpi_die("linuxkpi: cannot start the timer thread");
    timer_task = t;
}
