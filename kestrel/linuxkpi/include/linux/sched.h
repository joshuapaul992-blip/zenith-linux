/* linux/sched.h -- Kestrel's Linux KPI: threads as Linux sees them (linuxkpi/src/sched.c)
 *
 * Every Kestrel thread that runs Linux code gets a struct task_struct:
 * threads made by kthread_create() own one; any other thread (a process
 * in a system call, the boot thread) gets one kept per task slot.
 * set_current_state() + schedule() sleep as in Linux: the state is only
 * a request, and a wake_up_process() in between makes schedule() return
 * at once, so no wakeup is lost. */
#ifndef _LKPI_LINUX_SCHED_H
#define _LKPI_LINUX_SCHED_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/preempt.h>
#include <linux/refcount.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/smp.h>
#include <asm/barrier.h>

#define TASK_RUNNING            0x00000000
#define TASK_INTERRUPTIBLE      0x00000001
#define TASK_UNINTERRUPTIBLE    0x00000002
#define __TASK_STOPPED          0x00000004
#define __TASK_TRACED           0x00000008
#define TASK_PARKED             0x00000040
#define TASK_DEAD               0x00000080
#define TASK_WAKEKILL           0x00000100
#define TASK_WAKING             0x00000200
#define TASK_NOLOAD             0x00000400
#define TASK_NEW                0x00000800
#define TASK_FREEZABLE          0x00002000
#define TASK_KILLABLE           (TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_IDLE               (TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
#define TASK_NORMAL             (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

#define PF_EXITING              0x00000004
#define PF_KTHREAD              0x00200000
#define PF_MEMALLOC             0x00000800
#define PF_NOFREEZE             0x00008000

#define TASK_COMM_LEN           16
#define MAX_SCHEDULE_TIMEOUT    LONG_MAX

struct mm_struct;
struct lkpi_kthread;

struct task_struct {
    volatile unsigned int   __state;
    unsigned int            flags;              /* PF_* */
    pid_t                   pid, tgid;
    char                    comm[TASK_COMM_LEN];
    struct mm_struct       *mm;                 /* NULL: a kernel thread as far as Linux code knows */
    refcount_t              usage;
    int                     prio, static_prio;
    void                   *lkpi_thread;        /* Kestrel's thread */
    int                     lkpi_sleeping;      /* blocked in schedule()          */
    int                     lkpi_uaccess_kernel;/* user copies are kernel copies  */
    bool                    lkpi_owned;         /* freed at the last put          */
    struct lkpi_kthread    *lkpi_kthread;
};

struct task_struct *lkpi_get_current(void);
#define current                 lkpi_get_current()
#define get_current()           lkpi_get_current()

#define __set_current_state(s)  WRITE_ONCE(current->__state, (s))
#define set_current_state(s)    smp_store_mb(current->__state, (s))
#define get_current_state()     READ_ONCE(current->__state)
#define task_is_running(t)      (READ_ONCE((t)->__state) == TASK_RUNNING)

void lkpi_schedule(void);
long lkpi_schedule_timeout(long timeout);
static inline void schedule(void) { lkpi_schedule(); }
static inline long schedule_timeout(long timeout) { return lkpi_schedule_timeout(timeout); }
static inline long schedule_timeout_interruptible(long t) { __set_current_state(TASK_INTERRUPTIBLE); return schedule_timeout(t); }
static inline long schedule_timeout_uninterruptible(long t) { __set_current_state(TASK_UNINTERRUPTIBLE); return schedule_timeout(t); }
static inline long schedule_timeout_killable(long t) { __set_current_state(TASK_KILLABLE); return schedule_timeout(t); }
static inline long schedule_timeout_idle(long t) { __set_current_state(TASK_IDLE); return schedule_timeout(t); }
#define io_schedule()           schedule()
#define io_schedule_timeout(t)  schedule_timeout(t)
#define schedule_preempt_disabled() do { preempt_enable(); schedule(); preempt_disable(); } while (0)

int  wake_up_process(struct task_struct *t);
int  wake_up_state(struct task_struct *t, unsigned int state);
void lkpi_yield(void);
#define yield()                 lkpi_yield()
int  lkpi_cond_resched(void);
#define cond_resched()          lkpi_cond_resched()
#define cond_resched_lock(l)    0
#define need_resched()          0

int  lkpi_signal_pending(struct task_struct *t);
static inline int signal_pending(struct task_struct *t) { return lkpi_signal_pending(t); }
static inline int fatal_signal_pending(struct task_struct *t) { return lkpi_signal_pending(t); }
static inline int signal_pending_state(unsigned int state, struct task_struct *t)
{
    if (!(state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL))) return 0;
    return signal_pending(t);
}

static inline pid_t task_pid_nr(struct task_struct *t) { return t->pid; }
static inline pid_t task_tgid_nr(struct task_struct *t) { return t->tgid; }
static inline pid_t task_pid_vnr(struct task_struct *t) { return t->pid; }
#define get_task_comm(buf, t)   strscpy(buf, (t)->comm, sizeof(buf))
static inline struct task_struct *get_task_struct(struct task_struct *t) { refcount_inc(&t->usage); return t; }
void put_task_struct(struct task_struct *t);
#define sched_set_fifo(t)       do { (void)(t); } while (0)
#define sched_set_fifo_low(t)   do { (void)(t); } while (0)
#define sched_set_normal(t, n)  do { (void)(t); } while (0)
#define set_user_nice(t, n)     do { (void)(t); } while (0)
#define task_nice(t)            0

#endif
