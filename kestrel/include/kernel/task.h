/* include/kernel/task.h -- thread control blocks and the scheduler */
#ifndef KESTREL_TASK_H
#define KESTREL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_TASKS       64
#define KSTACK_SIZE     (16 * 1024)
#define TASK_NAME_LEN   24
#define MAX_FDS         16
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

/* Thread Control Block. Every kernel thread (and, later, every user
 * process' main thread) is described by one of these. */
struct tcb {
    int       pid, ppid;
    char      name[TASK_NAME_LEN];
    enum task_state state;

    /* CPU context: everything else lives on the kernel stack */
    uint64_t  rsp;              /* saved by context_switch()            */
    uint8_t  *kstack;           /* base of the kernel stack allocation   */
    size_t    kstack_size;
    uint64_t  cr3;              /* address space root (PML4 phys)        */

    /* scheduling */
    int       quantum;
    uint64_t  wake_tick;
    void     *wait_chan;
    uint64_t  cpu_ticks, switches, start_tick;
    int       exit_code;
    int       waiters;          /* task_wait() callers: not reaped yet   */

    /* POSIX process state */
    struct file  *fds[MAX_FDS];
    struct vnode *cwd;
    uint32_t  uid, gid;
    uint32_t  umask;
    int       tty;              /* system_ttys[] index: /dev/tty, stdin/stdout */

    /* user process (ring 3); all zero for kernel threads */
    bool      user;
    uint64_t  pml4;             /* own address space (uvm.h), 0 = kernel's */
    uint64_t  brk_start, brk;   /* program break                          */
    uint64_t  mmap_next;        /* next free address in the mmap region    */
    uint64_t  fs_base;          /* TLS pointer (arch_prctl ARCH_SET_FS)    */
    uint64_t  tid_address;      /* set_tid_address()                       */
    uint8_t   fpu[512] __attribute__((aligned(16)));    /* fxsave area     */
};

void        sched_init(void);
bool        sched_running(void);
void        sched_set_preemption(bool on);
bool        sched_preemption(void);
struct tcb *current_task(void);
struct tcb *task_create(const char *name, int (*entry)(void *), void *arg);
__attribute__((noreturn)) void task_exit(int code);
void        schedule(void);             /* call with interrupts disabled */
void        sched_yield(void);
void        sched_tick(void);           /* from the timer IRQ            */
void        sched_idle_loop(void) __attribute__((noreturn));

void        sleep_on(void *chan);       /* call with interrupts disabled */
void        wakeup(void *chan);
void        task_sleep_ms(uint64_t ms);
int         task_wait(int pid);         /* exit status, or -ECHILD        */

int         task_snapshot(struct tcb *out, int max);
const char *task_state_name(enum task_state s);
uint64_t    sched_context_switches(void);

#endif
