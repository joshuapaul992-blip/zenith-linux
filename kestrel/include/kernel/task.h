/* include/kernel/task.h -- thread control blocks and the scheduler */
#ifndef KESTREL_TASK_H
#define KESTREL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <kernel/kstat.h>
#include <kernel/spinlock.h>

#define MAX_TASKS       512             /* threads included */
#define KSTACK_SIZE     (16 * 1024)
#define TASK_NAME_LEN   24
#define MAX_FDS         1024            /* per process (RLIMIT_NOFILE) */
#define SCHED_QUANTUM   10              /* ticks (ms) per time slice */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,         /* runnable, waiting for the CPU            */
    TASK_RUNNING,       /* currently on the CPU                     */
    TASK_BLOCKED,       /* sleeping on a wait channel (I/O)          */
    TASK_SLEEPING,      /* timed sleep until wake_ms                 */
    TASK_ZOMBIE,        /* exited, waiting to be reaped              */
};

struct file;
struct vnode;
struct int_frame;

#define NSIG            64

/* Why a signal is pending: filled in when it is sent, copied into the
 * siginfo_t the handler gets (SA_SIGINFO) and into the sigcontext. */
struct ksig_info {
    int32_t  code;              /* si_code: SI_USER, SEGV_MAPERR, CLD_EXITED, ... */
    int32_t  pid;               /* sender / child                          */
    uint32_t uid;
    int32_t  status;            /* SIGCHLD: exit status or signal          */
    uint64_t addr;              /* faults: si_addr (and sigcontext cr2)    */
    uint64_t err;               /* faults: CPU error code                  */
    uint32_t trapno;            /* faults: exception vector                */
};

/* Kernel layout of struct sigaction (Linux x86_64 rt_sigaction). */
struct k_sigaction {
    uint64_t handler;           /* SIG_DFL 0, SIG_IGN 1, or a function     */
    uint64_t flags;             /* SA_*                                    */
    uint64_t restorer;          /* SA_RESTORER: calls rt_sigreturn          */
    uint64_t mask;              /* blocked while the handler runs          */
};

/* ---- what the threads of a process share (clone flags decide) ----------- */
struct files_struct {               /* CLONE_FILES: the descriptor table     */
    int          refs;
    struct spinlock lock;           /* fd[]: changes, and lookups taking a reference */
    struct file *fd[MAX_FDS];
    uint8_t      cloexec[MAX_FDS];
};

struct fs_struct {                  /* CLONE_FS: working directory, umask    */
    int           refs;
    struct vnode *cwd;
    uint32_t      umask;
};

struct sighand_struct {             /* CLONE_SIGHAND: signal actions         */
    int                refs;
    struct k_sigaction action[NSIG];
};

struct signal_struct {              /* CLONE_THREAD: the thread group        */
    int       refs;                 /* tcbs pointing here                    */
    int       nr_threads;           /* threads that have not exited          */
    uint64_t  shared_pending;       /* process-directed signals (kill)       */
    struct ksig_info shared_info[NSIG];
    bool      group_exit;           /* exit_group() or a fatal signal        */
    int       group_exit_code;
    int       group_term_signal;
    uint64_t  alarm_at, alarm_every;    /* ITIMER_REAL in ms (per process)   */
    void     *exit_wait;            /* execve waits here for the others      */
    bool      exec_kill;            /* execve is ending the other threads:
                                       their deaths are not the group's      */
    struct kstat_task dead;         /* counters of the threads that exited   */
};

/* Thread Control Block: one per kernel thread and per user thread. A user
 * process is a thread group: its threads share one tgid (= the leader's tid,
 * what getpid() returns) and the structures above. */
struct tcb {
    int       pid;              /* thread id (tid)                          */
    int       tgid;             /* thread group (process) id                */
    int       ppid;             /* parent process (tgid)                    */
    char      name[TASK_NAME_LEN];
    enum task_state state;

    /* CPU context: everything else lives on the kernel stack */
    uint64_t  rsp;              /* saved by context_switch()            */
    uint8_t  *kstack;           /* base of the kernel stack (a guard page lies below) */
    size_t    kstack_size;
    uint64_t  cr3;              /* address space root (PML4 phys)        */

    /* scheduling */
    int       quantum;
    bool      idle_class;       /* runs only when no other task is ready    */
    uint64_t  wake_ms;          /* uptime_ms() to wake a TASK_SLEEPING task */
    void     *wait_chan;
    struct poll_table *poll_tab;    /* poll/select/epoll: collecting, then sleeping on it */
    uint64_t  cpu_ticks, switches, start_tick;   /* start_tick: uptime in PIT_HZ ticks */
    int       exit_code;
    int       waiters;          /* task_wait() callers: not reaped yet   */

    /* POSIX process state (shared between threads per the clone flags) */
    struct files_struct   *files;
    struct fs_struct      *fs;
    struct sighand_struct *sighand;
    struct signal_struct  *signal;
    uint32_t  uid, gid;
    int       tty;              /* system_ttys[] index: /dev/tty, stdin/stdout */

    /* user process (ring 3); all zero for kernel threads */
    bool      user;
    struct mm *mm;              /* address space (vm.h); shared by vfork    */
    uint64_t  pml4;             /* = mm->pml4, 0 = the kernel's             */
    uint64_t  fs_base;          /* TLS pointer (arch_prctl ARCH_SET_FS)    */
    uint64_t  gs_base;          /* user GS base (arch_prctl ARCH_SET_GS)   */
    uint64_t  tid_address;      /* set_tid_address()                       */
    uint8_t   fpu[512] __attribute__((aligned(16)));    /* fxsave area     */

    /* processes (proc/process.c) */
    int       pgid, sid;        /* process group, session                  */
    int       ctty;             /* controlling terminal: pty index + 1      */
    char      exe[128];         /* program path (/proc/self/exe)           */
    char      cmdline[256];     /* argv, NUL-separated (/proc/PID/cmdline)  */
    uint16_t  cmdline_len;
    uint64_t  robust_list;      /* set_robust_list(): head in user memory  */
    int       exit_signal;      /* sent to the parent at exit (SIGCHLD)    */
    int       term_signal;      /* killed by this signal; 0 = exit()       */
    bool      oom_killed;       /* chosen by the OOM killer                 */
    int       vfork_parent;     /* pid blocked in vfork() until exec/exit   */
    struct int_frame *uframe;   /* user registers of the syscall in progress */
    bool      iret_return;      /* leave this syscall through iretq          */

    /* signals: mask and thread-directed pending set are per thread */
    uint64_t  sig_pending, sig_mask;
    uint64_t  saved_mask;       /* rt_sigsuspend: mask to restore           */
    bool      saved_mask_valid;
    struct ksig_info siginfo[NSIG];
    uint64_t  alt_sp, alt_size; /* sigaltstack(); alt_size 0 = none          */

    int       pagefault_off;    /* uaccess: page faults are not resolved (> 0) */
    int       bkl_depth;        /* big kernel lock nesting while switched out (smp.h) */
    bool      sched_cur;        /* some CPU's current task (under the scheduler lock) */
    int       on_cpu;           /* until its registers are saved at a switch-out    */
    struct kstat_task ks;       /* performance counters (kstat.h)            */
    int       fault_kind;       /* what the page fault in progress did (enum kfault) */
};

/* shared process state (proc/task.c) */
struct files_struct   *files_new(void);
struct files_struct   *files_dup(struct files_struct *src);    /* fork: same open files, own table */
void                   files_put(struct files_struct *f);       /* closes all at the last put */
struct fs_struct      *fs_new(struct vnode *cwd, uint32_t umask);
void                   fs_put(struct fs_struct *f);
struct sighand_struct *sighand_new(const struct sighand_struct *copy);
void                   sighand_put(struct sighand_struct *h);
struct signal_struct  *signal_new(void);
void                   signal_put(struct signal_struct *g);

void        sched_init(void);
bool        sched_running(void);
void        sched_set_preemption(bool on);
bool        sched_preemption(void);
struct tcb *current_task(void);
struct tcb *task_create(const char *name, int (*entry)(void *), void *arg);
struct tcb *task_create_paused(const char *name, int (*entry)(void *), void *arg);
void        task_start(struct tcb *t);  /* a paused task: runnable from now on */
__attribute__((noreturn)) void task_exit(int code);
void        schedule(void);             /* call with interrupts disabled */
void        sched_switch_tail(void);    /* the arriving task: its kernel lock back */
void        sched_yield(void);
void        task_set_idle_class(struct tcb *t);  /* background work (kworker/0) */
void        sched_tick(void);           /* boot CPU timer: sleepers, alarms, slice */
void        sched_tick_local(void);     /* other CPUs' timers: slice only          */
void        sched_tick_fast(void);      /* the same without the kernel lock         */
bool        sched_tick_needs_lock(void);    /* the boot CPU: an interval timer is due   */
void        sched_tick_boot_fast(void);     /* the boot CPU's tick without the lock     */
void        sched_alarm_armed(uint64_t at); /* ITIMER_REAL set to time_ms() `at`        */
void        sched_idle_loop(void) __attribute__((noreturn));

/* A sleeping, recursive mutex: the owner may lock it again (a page fault
 * inside a memory-management call); others sleep until it is free. */
struct kmutex {
    struct tcb *owner;
    int         depth;
};
void        kmutex_lock(struct kmutex *m);
void        kmutex_unlock(struct kmutex *m);
bool        kmutex_held(const struct kmutex *m);   /* by the calling task */

void        sleep_on(void *chan);       /* call with interrupts disabled */
/* sleep_on() for code under spinlock l (taken with interrupts off): the
 * task is asleep before l is dropped, so a waker that changes the state
 * under l and then calls wakeup(chan) cannot be missed; l is held again on
 * return. */
void        sleep_on_locked(void *chan, struct spinlock *l);
/* sleep_on() that also ends at deadline_ms of time_ms() (0: none) */
void        sleep_on_until(void *chan, uint64_t deadline_ms);
void        wakeup(void *chan);
/* Wake only the tasks sleeping on chan, not every poll()/epoll sleeper as
 * wakeup() does: for things nobody polls (mutexes, device completions). */
void        wakeup_waiters(void *chan);

/* Device waits. A driver polling its hardware for a completion calls
 * io_pause() between polls: the thread sleeps until io_wake(chan) (the
 * device's interrupt handler, or whatever noticed the completion) or the
 * next timer tick, so the CPU and the kernel lock serve other work
 * meanwhile. Where sleeping is impossible (interrupts off, no scheduler
 * yet, the idle thread) it busy-waits `us` microseconds instead. */
void        io_pause(void *chan, uint32_t us);
void        io_wake(void *chan);

/* Waiting for any of many objects (poll, select, epoll). Every wakeup()
 * advances a global sequence number: a poller reads the number, checks its
 * objects, and sleeps only if the number has not moved since -- no state
 * change can slip in between unnoticed.
 *
 * Which wakeups end the sleep: while the objects are checked (between
 * poll_begin and poll_end), each one's poll op names the channels its state
 * changes are announced on with poll_wait(chan), and the poller then wakes
 * only for wakeup() on one of those. An object whose poll op names none (or
 * more than POLL_CHANS in all) makes the poller wake at every wakeup(), as
 * every poller once did -- slower, never wrong.
 * poll_sleep returns at such a wakeup, a signal, or deadline_ms (0: none). */
#define POLL_CHANS 48
struct poll_table {
    bool      all;              /* wake at any wakeup()                       */
    uint16_t  n;
    uint32_t  calls;            /* poll_wait() calls, to spot silent poll ops */
    void     *chan[POLL_CHANS];
};
uint64_t    poll_seq_read(void);
void        poll_begin(struct poll_table *pt);
void        poll_wait(void *chan);  /* from a poll op: changes are announced on chan */
void        poll_end(void);
void        poll_sleep(uint64_t seq, uint64_t deadline_ms, struct poll_table *pt);
void        task_sleep_ms(uint64_t ms);
int         task_wait(int pid);         /* exit status, or -ECHILD        */

int         task_snapshot(struct tcb *out, int max);
struct tcb *task_find(int pid);         /* live or zombie, NULL if none     */
struct tcb *task_slot(int i);           /* 0 <= i < MAX_TASKS               */
void        task_reap(struct tcb *t);   /* free a zombie now (wait4)        */
void        task_interrupt(struct tcb *t);  /* end a sleep early (signals)  */
/* Futex-style sleep: mark the current task asleep on `chan` (until
 * task_wake, a signal, or deadline_ms of time_ms() if non-zero); it stops
 * running at the caller's next schedule(). Interrupts must be off. */
void        task_block_self(void *chan, uint64_t deadline_ms);
void        task_wake(struct tcb *t);       /* make a sleeping task runnable  */
bool        task_stack_guard_hit(const struct tcb *t, uint64_t addr);  /* in t's guard page */
const char *task_state_name(enum task_state s);
uint64_t    sched_context_switches(void);
void        sched_cpu_times(uint64_t *user, uint64_t *system, uint64_t *idle);

#endif
