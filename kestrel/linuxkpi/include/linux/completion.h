/* linux/completion.h -- Kestrel's Linux KPI: "this has happened" (linuxkpi/src/sched.c) */
#ifndef _LKPI_LINUX_COMPLETION_H
#define _LKPI_LINUX_COMPLETION_H

#include <linux/wait.h>

struct completion {
    unsigned int        done;           /* UINT_MAX after complete_all() */
    wait_queue_head_t   wait;
};

#define COMPLETION_INITIALIZER(work)    { 0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait) }
#define COMPLETION_INITIALIZER_ONSTACK(work) (*({ init_completion(&work); &work; }))
#define DECLARE_COMPLETION(work)        struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK(work) DECLARE_COMPLETION(work)

static inline void init_completion(struct completion *x) { x->done = 0; init_waitqueue_head(&x->wait); }
static inline void reinit_completion(struct completion *x) { x->done = 0; }

void complete(struct completion *x);
void complete_all(struct completion *x);
void wait_for_completion(struct completion *x);
/* the jiffies left (>= 1), or 0 on timeout */
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout);
int  wait_for_completion_interruptible(struct completion *x);
long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout);
int  wait_for_completion_killable(struct completion *x);
long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout);
#define wait_for_completion_io(x)               wait_for_completion(x)
#define wait_for_completion_io_timeout(x, t)    wait_for_completion_timeout(x, t)
#define wait_for_completion_state(x, s)         (wait_for_completion(x), 0)
bool try_wait_for_completion(struct completion *x);
bool completion_done(struct completion *x);

#endif
