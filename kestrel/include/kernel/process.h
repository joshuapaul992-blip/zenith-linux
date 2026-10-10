/* =============================================================================
 *  process.h -- POSIX processes: fork/vfork/clone, execve, wait4, sessions,
 *  and signals (proc/process.c)
 *
 *  Processes are the scheduler's tasks with t->user set. fork() copies the
 *  whole user address space (uvm_clone; no copy-on-write yet). vfork() and
 *  clone(CLONE_VM | CLONE_VFORK), musl's posix_spawn path, let the child
 *  borrow the parent's address space while the parent sleeps until the child
 *  calls execve() or exits. Threads (CLONE_THREAD) are not supported.
 *
 *  Signals follow Linux: 64 signals, per-process handlers and mask, the
 *  x86_64 rt_sigframe on the user stack (siginfo, ucontext with the saved
 *  registers and FPU state), return through the handler's SA_RESTORER
 *  trampoline into rt_sigreturn. Signals are delivered when the process
 *  returns to ring 3 (after a system call or an interrupt). Blocking calls
 *  give up when a signal is pending, with -ERESTARTSYS (restarted if the
 *  handler has SA_RESTART) or -ERESTARTNOHAND (always EINTR to a handler).
 *  No job control: stop signals are ignored. CPU faults in ring 3 still end
 *  the process at once (status 128 + signal) instead of calling a handler.
 * ============================================================================= */
#ifndef KESTREL_PROCESS_H
#define KESTREL_PROCESS_H

#include <stdint.h>
#include <stdbool.h>

struct tcb;
struct int_frame;

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGWINCH    28

#define SIG_DFL     0
#define SIG_IGN     1

#define SA_NOCLDSTOP    0x00000001
#define SA_NOCLDWAIT    0x00000002
#define SA_SIGINFO      0x00000004
#define SA_RESTORER     0x04000000
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000u

/* si_code values */
#define SI_USER         0
#define SI_KERNEL       0x80
#define SI_TKILL        (-6)
#define CLD_EXITED      1
#define CLD_KILLED      2

/* kernel-internal: never seen by user space */
#define ERESTARTSYS     512
#define ERESTARTNOHAND  514

/* ---- hooks for the scheduler -------------------------------------------- */
bool process_zombie_kept(const struct tcb *t);  /* parent will wait4() for it */
void process_exit(struct tcb *t);               /* t is exiting              */

/* ---- signals --------------------------------------------------------------- */
int  signal_send(struct tcb *t, int sig);       /* 0, or -ESRCH (sent by the kernel) */
struct ksig_info;
int  signal_send_info(struct tcb *t, int sig, const struct ksig_info *info);
/* The current process faulted (idt.c): deliver sig with this siginfo, even
 * if it is blocked or ignored (then the default action applies). */
void signal_force_fault(int sig, int code, uint64_t addr, uint32_t trapno, uint64_t err);
bool signal_pending(void);                      /* for blocking loops         */
/* Deliver pending signals to the current process before it returns to
 * ring 3 through `f` (interrupts off). `nr` is the system call being
 * finished, if `syscall`. May not return (default action: terminate). */
void signal_deliver(struct int_frame *f, uint64_t nr, bool syscall);

/* ---- system calls ------------------------------------------------------- */
int64_t proc_clone(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid, uint64_t tls);
int64_t proc_execve(uint64_t path, uint64_t argv, uint64_t envp);
int64_t proc_wait4(int64_t pid, uint64_t status, uint64_t options, uint64_t rusage);
int64_t proc_setsid(void);
int64_t proc_setpgid(int64_t pid, int64_t pgid);
int64_t proc_getpgid(int64_t pid);
int64_t proc_getsid(int64_t pid);

int64_t sig_action(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size);
int64_t sig_procmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size);
int64_t sig_pending_set(uint64_t set, uint64_t size);
int64_t sig_return(void);
int64_t sig_suspend(uint64_t mask, uint64_t size);
int64_t sig_pause(void);
int64_t sig_kill(int64_t pid, int64_t sig);
int64_t sig_tgkill(int64_t tgid, int64_t tid, int64_t sig);

#endif
