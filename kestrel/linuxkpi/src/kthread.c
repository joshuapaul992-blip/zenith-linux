/* linuxkpi/src/kthread.c -- kthread_create, kthread_stop and parking
 *
 * Each kthread is a Kestrel kernel thread plus a struct lkpi_kthread that
 * holds its task_struct. As in Linux the new thread does not run the
 * function until the first wake_up_process(), and a kthread_stop() before
 * that makes it exit with -EINTR without running it at all. The
 * task_struct is reference counted: the thread holds one reference until
 * it exits, kthread_stop() takes its own while it waits. */
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sprintf.h>
#include <linux/printk.h>
#include "internal.h"

struct lkpi_kthread {
    struct task_struct  task;           /* first: put_task_struct frees the whole */
    int               (*threadfn)(void *data);
    void               *data;
    int                 result;
    bool                should_stop;
    bool                should_park;
    bool                parked;
    struct completion   exited;
    struct completion   parked_done;
};

static struct lkpi_kthread *to_kthread(struct task_struct *k) { return k->lkpi_kthread; }

static __noreturn void kthread_finish(struct lkpi_kthread *k, int result)
{
    k->result = result;
    k->task.flags |= PF_EXITING;
    complete_all(&k->exited);
    lkpi_clear_current();
    put_task_struct(&k->task);
    lkpi_task_exit(0);
}

static int kthread_entry(void *arg)
{
    struct lkpi_kthread *k = arg;
    struct task_struct *ts = &k->task;

    ts->lkpi_thread = lkpi_task_self();
    ts->pid = ts->tgid = lkpi_task_pid(ts->lkpi_thread);
    lkpi_set_current(ts);

    /* wait for the first wake_up_process() (it sets TASK_RUNNING) */
    while (READ_ONCE(ts->__state) != TASK_RUNNING) schedule();
    if (READ_ONCE(k->should_stop)) kthread_finish(k, -EINTR);
    kthread_finish(k, k->threadfn(k->data));
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data), void *data, int node,
                                           const char namefmt[], ...)
{
    struct lkpi_kthread *k = kzalloc(sizeof *k, GFP_KERNEL);
    if (!k) return ERR_PTR(-ENOMEM);
    struct task_struct *ts = &k->task;
    va_list ap;
    va_start(ap, namefmt);
    vscnprintf(ts->comm, sizeof ts->comm, namefmt, ap);
    va_end(ap);
    ts->__state = TASK_UNINTERRUPTIBLE;
    ts->flags = PF_KTHREAD | PF_NOFREEZE;
    ts->prio = ts->static_prio = 120;
    refcount_set(&ts->usage, 1);
    ts->lkpi_owned = true;
    ts->lkpi_kthread = k;
    k->threadfn = threadfn;
    k->data = data;
    init_completion(&k->exited);
    init_completion(&k->parked_done);

    void *thread = lkpi_task_create(ts->comm, kthread_entry, k);
    if (!thread) {
        kfree(k);
        return ERR_PTR(-ENOMEM);
    }
    return ts;
}

bool kthread_should_stop(void)
{
    struct lkpi_kthread *k = to_kthread(current);
    return k && READ_ONCE(k->should_stop);
}

bool kthread_should_park(void)
{
    struct lkpi_kthread *k = to_kthread(current);
    return k && READ_ONCE(k->should_park);
}

bool kthread_freezable_should_stop(bool *was_frozen)
{
    if (was_frozen) *was_frozen = false;
    return kthread_should_stop();
}

void *kthread_data(struct task_struct *t)
{
    struct lkpi_kthread *k = to_kthread(t);
    return k ? k->data : NULL;
}

int kthread_stop(struct task_struct *t)
{
    struct lkpi_kthread *k = to_kthread(t);
    if (!k) return -EINVAL;
    get_task_struct(t);
    WRITE_ONCE(k->should_stop, true);
    if (READ_ONCE(k->parked)) kthread_unpark(t);
    wake_up_process(t);
    wait_for_completion(&k->exited);
    int ret = k->result;
    put_task_struct(t);
    return ret;
}

int kthread_stop_put(struct task_struct *t)
{
    int ret = kthread_stop(t);
    put_task_struct(t);
    return ret;
}

int kthread_park(struct task_struct *t)
{
    struct lkpi_kthread *k = to_kthread(t);
    if (!k || (t->flags & PF_EXITING)) return -ENOSYS;
    if (!READ_ONCE(k->parked)) {
        WRITE_ONCE(k->should_park, true);
        if (t != current) {
            wake_up_process(t);
            wait_for_completion(&k->parked_done);
        }
    }
    return 0;
}

void kthread_unpark(struct task_struct *t)
{
    struct lkpi_kthread *k = to_kthread(t);
    if (!k) return;
    WRITE_ONCE(k->should_park, false);
    reinit_completion(&k->parked_done);
    wake_up_state(t, TASK_PARKED);
}

void kthread_parkme(void)
{
    struct lkpi_kthread *k = to_kthread(current);
    if (!k) return;
    for (;;) {
        set_current_state(TASK_PARKED);
        if (!READ_ONCE(k->should_park)) break;
        if (!k->parked) {
            k->parked = true;
            complete(&k->parked_done);
        }
        schedule();
    }
    k->parked = false;
    __set_current_state(TASK_RUNNING);
}

void kthread_exit(long result)
{
    struct lkpi_kthread *k = to_kthread(current);
    if (!k) lkpi_die("linuxkpi: kthread_exit outside a kthread");
    kthread_finish(k, (int)result);
}

void kthread_complete_and_exit(struct completion *c, long result)
{
    if (c) complete(c);
    kthread_exit(result);
}
