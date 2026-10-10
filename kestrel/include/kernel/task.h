/* include/kernel/task.h -- thread control blocks and the scheduler */
#ifndef KESTREL_TASK_H
#define KESTREL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TASKS       256             /* threads included */
#define KSTACK_SIZE     (16 * 1024)
#define TASK_NAME_LEN   24
#define MAX_FDS         64
#define SCHED_QUANTUM   10              /* ticks (ms) per time slice */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,         /* runnable, waiting for the CPU            */
    TASK_RUNNING,       /* currently on the CPU                     */
    TASK_BLOCKED,       /* sleeping on a wait channel (I/O)          */
    TASK_SLEEPING,      /* timed sleep until wake_tick               */
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
    uint64_t  wake_tick;
    void     *wait_chan;
    uint64_t  cpu_ticks, switches, start_tick;
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

    int       pagefault_off;    /* uaccess: page faults are not resolved (> 0) */
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
__attribute__((noreturn)) void task_exit(int code);
void        schedule(void);             /* call with interrupts disabled */
void        sched_yield(void);
void        task_set_idle_class(struct tcb *t);  /* background work (kworker/0) */
void        sched_tick(void);           /* from the timer IRQ            */
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
void        wakeup(void *chan);
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
