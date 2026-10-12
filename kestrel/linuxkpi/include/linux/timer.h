/* linux/timer.h -- Kestrel's Linux KPI: timer_list (linuxkpi/src/timer.c)
 *
 * Pending timers sit on one list ordered by expiry; a kernel thread
 * ("lkpi-timer") sleeps until the first is due and runs the callbacks,
 * one at a time. A callback must not sleep, as in Linux. */
#ifndef _LKPI_LINUX_TIMER_H
#define _LKPI_LINUX_TIMER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/container_of.h>

struct timer_list {
    struct list_head    entry;          /* on the pending list (entry.prev == NULL: not pending) */
    unsigned long       expires;
    void              (*function)(struct timer_list *t);
    u32                 flags;
};

#define TIMER_DEFERRABLE        0x00080000
#define TIMER_PINNED            0x00100000
#define TIMER_IRQSAFE           0x00200000

void lkpi_timer_init(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags);
#define timer_setup(timer, callback, flags)     lkpi_timer_init((timer), (callback), (flags))
#define timer_setup_on_stack(timer, cb, flags)  lkpi_timer_init((timer), (cb), (flags))
#define destroy_timer_on_stack(timer)           do { } while (0)
#define from_timer(var, callback_timer, timer_fieldname) \
    container_of(callback_timer, typeof(*var), timer_fieldname)
#define timer_container_of(var, t, field)       from_timer(var, t, field)
#define DEFINE_TIMER(name, fn) \
    struct timer_list name = { .entry = { NULL, NULL }, .function = (fn) }

static inline int timer_pending(const struct timer_list *t) { return READ_ONCE(t->entry.prev) != NULL; }
void add_timer(struct timer_list *t);
/* (re)arm for `expires`: 1 if it was pending, else 0 */
int  mod_timer(struct timer_list *t, unsigned long expires);
int  mod_timer_pending(struct timer_list *t, unsigned long expires);
int  timer_reduce(struct timer_list *t, unsigned long expires);
/* disarm: 1 if it was pending; the _sync forms also wait for a running callback */
int  timer_delete(struct timer_list *t);
int  timer_delete_sync(struct timer_list *t);
int  timer_shutdown_sync(struct timer_list *t);
int  timer_shutdown(struct timer_list *t);
#define del_timer(t)            timer_delete(t)
#define del_timer_sync(t)       timer_delete_sync(t)
#define del_singleshot_timer_sync(t) timer_delete_sync(t)
#define add_timer_on(t, cpu)    add_timer(t)

#endif
