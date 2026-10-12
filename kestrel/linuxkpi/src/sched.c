/* linuxkpi/src/sched.c -- threads, sleeping and the locks built on it
 *
 * current: Kestrel threads have no struct task_struct, so the KPI keeps
 * one per task slot. Threads made by kthread_create() bring their own;
 * any other thread that runs Linux code (a process in a system call, the
 * boot thread) gets a per-slot one, reset when the slot changes hands.
 *
 * Sleeping follows Linux: set_current_state() records the wish to sleep,
 * schedule() sleeps only if the state is still not TASK_RUNNING, and a
 * wakeup sets it back to TASK_RUNNING. One lock (sleep_lock) orders the
 * two sides, so a wakeup that lands between the state change and the
 * sleep is never lost.
 *
 * Preemption: a per-CPU count, with interrupts off while it is non-zero
 * (Kestrel preempts kernel threads from the timer interrupt; with
 * interrupts off nothing can). local_irq_enable() inside such a section
 * is deferred to its end.
 *
 * On top: wait queues, completions, mutexes, rw semaphores, counting
 * semaphores and a small RCU. */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/semaphore.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include "internal.h"

/* ---- per-CPU state -------------------------------------------------------- */
struct lkpi_cpu {
    int             preempt;        /* preempt_disable() depth                  */
    unsigned long   saved_flags;    /* interrupt state when it went 0 -> 1      */
    int             irq_pending;    /* local_irq_enable() inside a section      */
    int             hardirq;        /* inside a KPI interrupt handler           */
    unsigned long   qs;             /* sections ended: RCU's quiescent states   */
};
static struct lkpi_cpu cpus[LKPI_MAX_CPUS];

static struct lkpi_cpu *this_cpu(void)     /* interrupts off */
{
    int id = lkpi_cpu_id();
    if ((unsigned)id >= LKPI_MAX_CPUS) lkpi_die("linuxkpi: CPU number out of range");
    return &cpus[id];
}

void lkpi_preempt_disable(void)
{
    unsigned long flags = arch_local_irq_save();
    struct lkpi_cpu *c = this_cpu();
    if (c->preempt++ == 0) c->saved_flags = flags;
}

void lkpi_preempt_enable(void)
{
    struct lkpi_cpu *c = this_cpu();               /* interrupts are off in a section */
    if (c->preempt <= 0) {
        printk(KERN_ERR "linuxkpi: preempt_enable() without preempt_disable()\n");
        return;
    }
    if (--c->preempt) return;
    c->qs++;
    unsigned long flags = c->saved_flags;
    if (c->irq_pending) { c->irq_pending = 0; flags |= X86_EFLAGS_IF; }
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
}

int lkpi_preempt_count(void)
{
    unsigned long flags = arch_local_irq_save();
    int n = this_cpu()->preempt;
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    return n;
}

void lkpi_local_irq_enable(void)
{
    arch_local_irq_disable();
    struct lkpi_cpu *c = this_cpu();
    if (c->preempt) c->irq_pending = 1;
    else arch_local_irq_enable();
}

int lkpi_in_hardirq(void)
{
    unsigned long flags = arch_local_irq_save();
    int n = this_cpu()->hardirq;
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    return n != 0;
}

/* around a KPI interrupt handler (interrupts off) */
void lkpi_hardirq_enter(void) { this_cpu()->hardirq++; }
void lkpi_hardirq_exit(void) { this_cpu()->hardirq--; }

int lkpi_smp_processor_id(void)
{
    unsigned long flags = arch_local_irq_save();
    int id = lkpi_cpu_id();
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    return id;
}

int lkpi_num_online_cpus(void) { return lkpi_cpu_count(); }

/* ---- current ---------------------------------------------------------------- */
struct slot_map {
    void               *thread;     /* the Kestrel thread it was made for */
    int                 pid;
    struct task_struct *ts;
};
static struct slot_map *slots;                      /* lkpi_task_slots() entries */
static struct task_struct *slot_tasks;              /* the per-slot task_structs */
static struct task_struct idle_tasks[LKPI_MAX_CPUS];

static void task_init(struct task_struct *ts, void *thread)
{
    memset(ts, 0, sizeof *ts);
    ts->__state = TASK_RUNNING;
    ts->pid = ts->tgid = thread ? lkpi_task_pid(thread) : 0;
    if (thread) strscpy(ts->comm, lkpi_task_name(thread), sizeof ts->comm);
    else strscpy(ts->comm, "idle", sizeof ts->comm);
    /* Linux code tells user threads by ->mm; nothing may look inside it */
    ts->mm = thread && lkpi_task_is_user(thread) ? (struct mm_struct *)&ts->mm : NULL;
    refcount_set(&ts->usage, 1);
    ts->prio = ts->static_prio = 120;
    ts->lkpi_thread = thread;
}

void lkpi_sched_init(void)
{
    int n = lkpi_task_slots();
    slots = lkpi_heap_alloc(n * sizeof *slots);
    slot_tasks = lkpi_heap_alloc(n * sizeof *slot_tasks);
    if (!slots || !slot_tasks) lkpi_die("linuxkpi: no memory for the task table");
    memset(slots, 0, n * sizeof *slots);
    memset(slot_tasks, 0, n * sizeof *slot_tasks);
}

struct task_struct *lkpi_get_current(void)
{
    unsigned long flags = arch_local_irq_save();
    void *self = lkpi_task_self();
    int slot = lkpi_task_slot(self);
    struct task_struct *ts;

    if (slot < 0) {                                 /* a CPU's idle thread */
        ts = &idle_tasks[lkpi_cpu_id() % LKPI_MAX_CPUS];
        if (ts->lkpi_thread != self) task_init(ts, NULL), ts->lkpi_thread = self;
    } else {
        struct slot_map *m = &slots[slot];
        int pid = lkpi_task_pid(self);
        if (m->thread != self || m->pid != pid || !m->ts) {
            ts = &slot_tasks[slot];
            task_init(ts, self);
            m->thread = self;
            m->pid = pid;
            m->ts = ts;
        }
        ts = m->ts;
    }
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    return ts;
}

/* kthread.c: the calling thread is ts (its own task_struct) */
void lkpi_set_current(struct task_struct *ts)
{
    unsigned long flags = arch_local_irq_save();
    void *self = lkpi_task_self();
    int slot = lkpi_task_slot(self);
    if (slot >= 0) {
        slots[slot].thread = self;
        slots[slot].pid = lkpi_task_pid(self);
        slots[slot].ts = ts;
    }
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
}

void lkpi_clear_current(void)
{
    unsigned long flags = arch_local_irq_save();
    int slot = lkpi_task_slot(lkpi_task_self());
    if (slot >= 0) slots[slot].ts = NULL;
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
}

void put_task_struct(struct task_struct *t)
{
    if (refcount_dec_and_test(&t->usage) && t->lkpi_owned) kfree(t->lkpi_kthread);
}

/* ---- sleeping and waking ------------------------------------------------------ */
static raw_spinlock_t sleep_lock = __RAW_SPIN_LOCK_UNLOCKED(sleep_lock);

long lkpi_schedule_timeout(long timeout)
{
    struct task_struct *ts = current;
    unsigned long long deadline = 0;

    if (timeout < 0) {
        printk(KERN_ERR "linuxkpi: schedule_timeout: wrong timeout value %lx\n", timeout);
        __set_current_state(TASK_RUNNING);
        return 0;
    }
    if (lkpi_preempt_count() || lkpi_in_hardirq()) {
        printk(KERN_ERR "linuxkpi: BUG: scheduling while atomic in %s\n", ts->comm);
        __set_current_state(TASK_RUNNING);
        return timeout;
    }
    if (timeout != MAX_SCHEDULE_TIMEOUT) deadline = lkpi_time_ms() + (unsigned long long)timeout;

    unsigned long flags = arch_local_irq_save();
    __lkpi_raw_lock(&sleep_lock);
    unsigned int state = ts->__state;
    if (state == TASK_RUNNING || !timeout || signal_pending_state(state, ts)) {
        ts->__state = TASK_RUNNING;
        __lkpi_raw_unlock(&sleep_lock);
        if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
        return timeout;
    }
    ts->lkpi_sleeping = 1;
    lkpi_task_block(ts, deadline);
    __lkpi_raw_unlock(&sleep_lock);
    lkpi_task_switch();                             /* returns woken, timed out or interrupted */
    __lkpi_raw_lock(&sleep_lock);
    ts->lkpi_sleeping = 0;
    ts->__state = TASK_RUNNING;
    __lkpi_raw_unlock(&sleep_lock);
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();

    if (timeout == MAX_SCHEDULE_TIMEOUT) return timeout;
    unsigned long long now = lkpi_time_ms();
    return now >= deadline ? 0 : (long)(deadline - now);
}

void lkpi_schedule(void) { lkpi_schedule_timeout(MAX_SCHEDULE_TIMEOUT); }

static int try_to_wake_up(struct task_struct *ts, unsigned int state)
{
    unsigned long flags = arch_local_irq_save();
    __lkpi_raw_lock(&sleep_lock);
    int woken = 0;
    if (ts->__state & state) {
        ts->__state = TASK_RUNNING;
        if (ts->lkpi_sleeping) lkpi_task_wake(ts->lkpi_thread);
        woken = 1;
    }
    __lkpi_raw_unlock(&sleep_lock);
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    return woken;
}

int wake_up_process(struct task_struct *t) { return try_to_wake_up(t, TASK_NORMAL); }
int wake_up_state(struct task_struct *t, unsigned int state) { return try_to_wake_up(t, state); }

void lkpi_yield(void) { lkpi_task_yield(); }

int lkpi_cond_resched(void)
{
    if (lkpi_preempt_count() || lkpi_in_hardirq()) return 0;
    lkpi_task_yield();
    return 1;
}

int lkpi_signal_pending(struct task_struct *t)
{
    return t == current && t->mm && lkpi_task_signal_pending();
}

/* ---- wait queues ------------------------------------------------------------------ */
int default_wake_function(struct wait_queue_entry *e, unsigned mode, int flags, void *key)
{
    return try_to_wake_up(e->private, mode);
}

int autoremove_wake_function(struct wait_queue_entry *e, unsigned mode, int flags, void *key)
{
    int r = default_wake_function(e, mode, flags, key);
    if (r) list_del_init(&e->entry);
    return r;
}

int woken_wake_function(struct wait_queue_entry *e, unsigned mode, int flags, void *key)
{
    smp_mb();
    e->flags |= WQ_FLAG_WOKEN;
    return default_wake_function(e, mode, flags, key);
}

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    unsigned long flags;
    e->flags &= ~WQ_FLAG_EXCLUSIVE;
    spin_lock_irqsave(&wq->lock, flags);
    list_add(&e->entry, &wq->head);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    unsigned long flags;
    e->flags |= WQ_FLAG_EXCLUSIVE;
    spin_lock_irqsave(&wq->lock, flags);
    list_add_tail(&e->entry, &wq->head);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    list_del(&e->entry);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
    unsigned long flags;
    e->flags &= ~WQ_FLAG_EXCLUSIVE;
    spin_lock_irqsave(&wq->lock, flags);
    if (list_empty(&e->entry)) list_add(&e->entry, &wq->head);
    set_current_state(state);
    spin_unlock_irqrestore(&wq->lock, flags);
}

bool prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
    unsigned long flags;
    bool was_empty = false;
    e->flags |= WQ_FLAG_EXCLUSIVE;
    spin_lock_irqsave(&wq->lock, flags);
    if (list_empty(&e->entry)) {
        was_empty = list_empty(&wq->head);
        list_add_tail(&e->entry, &wq->head);
    }
    set_current_state(state);
    spin_unlock_irqrestore(&wq->lock, flags);
    return was_empty;
}

long prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e, int state)
{
    unsigned long flags;
    long ret = 0;
    spin_lock_irqsave(&wq->lock, flags);
    if (signal_pending_state(state, current)) {
        /* an exclusive waiter that was woken passes the wakeup on (__wake_up stopped at it) */
        list_del_init(&e->entry);
        ret = -ERESTARTSYS;
    } else {
        if (list_empty(&e->entry)) {
            if (e->flags & WQ_FLAG_EXCLUSIVE) list_add_tail(&e->entry, &wq->head);
            else list_add(&e->entry, &wq->head);
        }
        set_current_state(state);
    }
    spin_unlock_irqrestore(&wq->lock, flags);
    return ret;
}

void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    unsigned long flags;
    __set_current_state(TASK_RUNNING);
    if (!list_empty_careful(&e->entry)) {
        spin_lock_irqsave(&wq->lock, flags);
        list_del_init(&e->entry);
        spin_unlock_irqrestore(&wq->lock, flags);
    }
}

long wait_woken(struct wait_queue_entry *e, unsigned mode, long timeout)
{
    set_current_state(mode);
    if (!(e->flags & WQ_FLAG_WOKEN) && !kthread_should_stop()) timeout = schedule_timeout(timeout);
    __set_current_state(TASK_RUNNING);
    smp_store_mb(e->flags, e->flags & ~WQ_FLAG_WOKEN);
    return timeout;
}

static void wake_up_common(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
    struct wait_queue_entry *curr, *next;
    list_for_each_entry_safe(curr, next, &wq->head, entry) {
        unsigned int flags = curr->flags;
        int r = curr->func(curr, mode, 0, key);
        if (r < 0) break;
        if (r && (flags & WQ_FLAG_EXCLUSIVE) && !--nr) break;
    }
}

void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key)
{
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    wake_up_common(wq, mode, nr, key);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void __wake_up_locked(struct wait_queue_head *wq, unsigned int mode, int nr)
{
    wake_up_common(wq, mode, nr, NULL);
}

/* ---- completions ------------------------------------------------------------------- */
void complete(struct completion *x)
{
    unsigned long flags;
    spin_lock_irqsave(&x->wait.lock, flags);
    if (x->done != UINT_MAX) x->done++;
    __wake_up_locked(&x->wait, TASK_NORMAL, 1);
    spin_unlock_irqrestore(&x->wait.lock, flags);
}

void complete_all(struct completion *x)
{
    unsigned long flags;
    spin_lock_irqsave(&x->wait.lock, flags);
    x->done = UINT_MAX;
    __wake_up_locked(&x->wait, TASK_NORMAL, 0);
    spin_unlock_irqrestore(&x->wait.lock, flags);
}

/* the time left (>= 1) once done, 0 on timeout, -ERESTARTSYS on a signal */
static long wait_common(struct completion *x, long timeout, unsigned int state)
{
    might_sleep();
    spin_lock_irq(&x->wait.lock);
    if (!x->done) {
        DECLARE_WAITQUEUE(wait, current);
        wait.flags |= WQ_FLAG_EXCLUSIVE;
        list_add_tail(&wait.entry, &x->wait.head);
        do {
            if (signal_pending_state(state, current)) { timeout = -ERESTARTSYS; break; }
            __set_current_state(state);
            spin_unlock_irq(&x->wait.lock);
            timeout = schedule_timeout(timeout);
            spin_lock_irq(&x->wait.lock);
        } while (!x->done && timeout);
        list_del(&wait.entry);
        if (!x->done) {
            spin_unlock_irq(&x->wait.lock);
            return timeout;
        }
    }
    if (x->done != UINT_MAX) x->done--;
    spin_unlock_irq(&x->wait.lock);
    return timeout ? timeout : 1;
}

void wait_for_completion(struct completion *x) { wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE); }

unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
    return (unsigned long)wait_common(x, (long)timeout, TASK_UNINTERRUPTIBLE);
}

int wait_for_completion_interruptible(struct completion *x)
{
    long r = wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
    return r == -ERESTARTSYS ? (int)r : 0;
}

long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout)
{
    return wait_common(x, (long)timeout, TASK_INTERRUPTIBLE);
}

int wait_for_completion_killable(struct completion *x)
{
    long r = wait_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
    return r == -ERESTARTSYS ? (int)r : 0;
}

long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout)
{
    return wait_common(x, (long)timeout, TASK_KILLABLE);
}

bool try_wait_for_completion(struct completion *x)
{
    unsigned long flags;
    bool ret = true;
    if (!READ_ONCE(x->done)) return false;
    spin_lock_irqsave(&x->wait.lock, flags);
    if (!x->done) ret = false;
    else if (x->done != UINT_MAX) x->done--;
    spin_unlock_irqrestore(&x->wait.lock, flags);
    return ret;
}

bool completion_done(struct completion *x)
{
    unsigned long flags;
    if (!READ_ONCE(x->done)) return false;
    spin_lock_irqsave(&x->wait.lock, flags);       /* complete() may still be inside */
    spin_unlock_irqrestore(&x->wait.lock, flags);
    return true;
}

/* ---- mutexes: an owner and a wait queue --------------------------------------------- */
int mutex_trylock(struct mutex *m)
{
    return cmpxchg(&m->owner, NULL, current) == NULL;
}

static int mutex_lock_common(struct mutex *m, unsigned int state)
{
    might_sleep();
    if (mutex_trylock(m)) return 0;
    DEFINE_WAIT(wait);
    wait.flags |= WQ_FLAG_EXCLUSIVE;
    int ret = 0;
    for (;;) {
        if (prepare_to_wait_event(&m->wait, &wait, state)) { ret = -EINTR; break; }
        if (mutex_trylock(m)) break;
        schedule();
    }
    finish_wait(&m->wait, &wait);
    if (ret) wake_up(&m->wait);                     /* the wakeup may have been ours */
    return ret;
}

void mutex_lock(struct mutex *m) { mutex_lock_common(m, TASK_UNINTERRUPTIBLE); }
int mutex_lock_interruptible(struct mutex *m) { return mutex_lock_common(m, TASK_INTERRUPTIBLE); }
int mutex_lock_killable(struct mutex *m) { return mutex_lock_common(m, TASK_KILLABLE); }

void mutex_unlock(struct mutex *m)
{
    if (READ_ONCE(m->owner) != current)
        printk(KERN_ERR "linuxkpi: mutex_unlock by %s, which does not hold it\n", current->comm);
    smp_store_release(&m->owner, NULL);
    smp_mb();
    if (waitqueue_active(&m->wait)) wake_up(&m->wait);
}

int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
    if (atomic_add_unless(cnt, -1, 1)) return 0;
    mutex_lock(lock);
    if (!atomic_dec_and_test(cnt)) { mutex_unlock(lock); return 0; }
    return 1;
}

/* ---- rw semaphores: count > 0 readers, -1 a writer -------------------------------- */
int down_read_trylock(struct rw_semaphore *s)
{
    long c = atomic_long_read(&s->count);
    while (c >= 0)
        if (atomic_long_try_cmpxchg(&s->count, &c, c + 1)) return 1;
    return 0;
}

int down_write_trylock(struct rw_semaphore *s)
{
    long c = 0;
    return atomic_long_try_cmpxchg(&s->count, &c, -1);
}

void down_read(struct rw_semaphore *s) { wait_event(s->wait, down_read_trylock(s)); }
int down_read_interruptible(struct rw_semaphore *s) { return wait_event_interruptible(s->wait, down_read_trylock(s)) ? -EINTR : 0; }
int down_read_killable(struct rw_semaphore *s) { return wait_event_killable(s->wait, down_read_trylock(s)) ? -EINTR : 0; }
void down_write(struct rw_semaphore *s) { wait_event(s->wait, down_write_trylock(s)); }
int down_write_killable(struct rw_semaphore *s) { return wait_event_killable(s->wait, down_write_trylock(s)) ? -EINTR : 0; }

void up_read(struct rw_semaphore *s)
{
    if (atomic_long_dec_return(&s->count) == 0) wake_up_all(&s->wait);
}

void up_write(struct rw_semaphore *s)
{
    atomic_long_set(&s->count, 0);
    wake_up_all(&s->wait);
}

void downgrade_write(struct rw_semaphore *s)
{
    atomic_long_set(&s->count, 1);
    wake_up_all(&s->wait);
}

/* ---- counting semaphores ------------------------------------------------------------ */
static bool sem_take(struct semaphore *s)
{
    unsigned long flags;
    bool ok = false;
    raw_spin_lock_irqsave(&s->lock, flags);
    if (s->count > 0) { s->count--; ok = true; }
    raw_spin_unlock_irqrestore(&s->lock, flags);
    return ok;
}

void down(struct semaphore *s) { wait_event_exclusive_kpi(s->wait, sem_take(s)); }
int down_trylock(struct semaphore *s) { return !sem_take(s); }

int down_interruptible(struct semaphore *s)
{
    if (wait_event_interruptible_exclusive(s->wait, sem_take(s))) { wake_up(&s->wait); return -EINTR; }
    return 0;
}

int down_killable(struct semaphore *s)
{
    if (wait_event_killable(s->wait, sem_take(s))) return -EINTR;
    return 0;
}

int down_timeout(struct semaphore *s, long timeout)
{
    return wait_event_timeout(s->wait, sem_take(s), timeout) ? 0 : -ETIME;
}

void up(struct semaphore *s)
{
    unsigned long flags;
    raw_spin_lock_irqsave(&s->lock, flags);
    s->count++;
    raw_spin_unlock_irqrestore(&s->lock, flags);
    wake_up(&s->wait);
}

/* ---- RCU ---------------------------------------------------------------------------
 * Readers have preemption (and so interrupts) off. A grace period has
 * passed once every CPU that was inside a section when it began has left
 * it: each CPU counts the sections it leaves, so the waiter compares. */
void synchronize_rcu(void)
{
    unsigned long snap[LKPI_MAX_CPUS];
    bool busy[LKPI_MAX_CPUS];
    int n = lkpi_cpu_count(), self = -1;

    might_sleep();
    unsigned long flags = arch_local_irq_save();
    if (!this_cpu()->preempt) self = lkpi_cpu_id();
    if (!arch_irqs_disabled_flags(flags)) arch_local_irq_enable();
    for (int i = 0; i < n && i < LKPI_MAX_CPUS; i++) {
        snap[i] = READ_ONCE(cpus[i].qs);
        busy[i] = i != self && READ_ONCE(cpus[i].preempt) > 0;
    }
    smp_mb();
    for (int i = 0; i < n && i < LKPI_MAX_CPUS; i++)
        while (busy[i] && READ_ONCE(cpus[i].qs) == snap[i] && READ_ONCE(cpus[i].preempt) > 0)
            lkpi_task_yield();
}

static struct rcu_head *rcu_list;                   /* call_rcu callbacks waiting */
static DEFINE_SPINLOCK(rcu_lock);
static void rcu_work_fn(struct work_struct *w);
static DECLARE_WORK(rcu_work, rcu_work_fn);

#define RCU_KFREE_MAX_OFFSET    4096                /* kfree_rcu: func holds the offset */

static void rcu_work_fn(struct work_struct *w)
{
    unsigned long flags;
    spin_lock_irqsave(&rcu_lock, flags);
    struct rcu_head *list = rcu_list;
    rcu_list = NULL;
    spin_unlock_irqrestore(&rcu_lock, flags);
    if (!list) return;
    synchronize_rcu();
    while (list) {
        struct rcu_head *next = list->next;
        unsigned long off = (unsigned long)list->func;
        if (off < RCU_KFREE_MAX_OFFSET) kfree((char *)list - off);
        else list->func(list);
        list = next;
    }
}

void call_rcu(struct rcu_head *head, rcu_callback_t func)
{
    unsigned long flags;
    head->func = func;
    spin_lock_irqsave(&rcu_lock, flags);
    head->next = rcu_list;
    rcu_list = head;
    spin_unlock_irqrestore(&rcu_lock, flags);
    schedule_work(&rcu_work);
}

void lkpi_kfree_rcu(void *obj, struct rcu_head *head)
{
    unsigned long off = (unsigned long)((char *)head - (char *)obj);
    if (off >= RCU_KFREE_MAX_OFFSET) {              /* too far in: wait here instead */
        synchronize_rcu();
        kfree(obj);
        return;
    }
    call_rcu(head, (rcu_callback_t)off);
}

void rcu_barrier(void)
{
    do flush_work(&rcu_work); while (READ_ONCE(rcu_list));
}
