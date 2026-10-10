/* proc/task.c -- kernel threads and a preemptive round-robin scheduler.
 *
 * Every thread owns a 16 KiB kernel stack. A switch saves callee-saved
 * registers on the outgoing stack (context_switch in switch.asm) and
 * resumes the incoming thread wherever it last called schedule() -- which,
 * for a preempted thread, is inside the timer interrupt handler. When the
 * handler returns, iretq restores the thread's full user/kernel state. */
#include <kernel/task.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/uvm.h>
#include <kernel/process.h>

extern void context_switch(uint64_t *save_rsp, uint64_t load_rsp);
extern void thread_trampoline(void);
extern uint64_t syscall_kernel_rsp;
extern uint8_t boot_stack[], boot_stack_top[];

static struct tcb tasks[MAX_TASKS];
static struct tcb *cur;
static int next_pid = 1;
static bool running, preempt = true, need_resched;
static uint64_t nswitches;

struct tcb *current_task(void) { return cur; }
bool sched_running(void) { return running; }
void sched_set_preemption(bool on) { preempt = on; }
bool sched_preemption(void) { return preempt; }
uint64_t sched_context_switches(void) { return nswitches; }

/* Whole-system CPU time in timer ticks, for /proc/stat: ticks spent in the
 * idle thread, in ring-3 processes (including their system calls) and in
 * kernel threads. */
static uint64_t ticks_user, ticks_system, ticks_idle;
void sched_cpu_times(uint64_t *user, uint64_t *system, uint64_t *idle)
{
    *user = ticks_user; *system = ticks_system; *idle = ticks_idle;
}

const char *task_state_name(enum task_state s)
{
    static const char *n[] = { "unused", "ready", "running", "blocked", "sleeping", "zombie" };
    return s <= TASK_ZOMBIE ? n[s] : "?";
}

void sched_init(void)
{
    /* Adopt the boot thread as pid 0. It runs the boot manager and then
     * becomes the idle thread. */
    struct tcb *t = &tasks[0];
    memset(t, 0, sizeof *t);
    t->pid = 0;
    strlcpy(t->name, "swapper", sizeof t->name);
    t->state = TASK_RUNNING;
    t->kstack = boot_stack;
    t->kstack_size = (size_t)(boot_stack_top - boot_stack);
    t->cr3 = read_cr3();
    t->quantum = SCHED_QUANTUM;
    cur = t;
    tss_set_kernel_stack((uint64_t)boot_stack_top);
    syscall_kernel_rsp = (uint64_t)boot_stack_top;
    running = true;
    kprintf("sched: pid 0 (swapper) adopted boot stack, quantum=%dms\n", SCHED_QUANTUM);
}

static void reap_one(struct tcb *t)
{
    for (int fd = 0; fd < MAX_FDS; fd++) if (t->fds[fd]) vfs_close(t->fds[fd]);
    if (t->pml4 && !t->vm_borrowed) uvm_destroy(t->pml4);
    t->pml4 = 0;
    kfree(t->kstack);
    t->state = TASK_UNUSED;
}

/* Zombies go away here unless someone still wants their exit status: a
 * task_wait() caller, or a parent process that has not called wait4(). */
static void reap_zombies(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        struct tcb *t = &tasks[i];
        if (t->state != TASK_ZOMBIE || t == cur || t->waiters || process_zombie_kept(t)) continue;
        reap_one(t);
    }
}

void task_reap(struct tcb *t)
{
    uint64_t f = irq_save();
    if (t->state == TASK_ZOMBIE && t != cur && !t->waiters) reap_one(t);
    irq_restore(f);
}

struct tcb *task_find(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) return &tasks[i];
    return NULL;
}

struct tcb *task_slot(int i) { return (i >= 0 && i < MAX_TASKS) ? &tasks[i] : NULL; }

void task_interrupt(struct tcb *t)
{
    if (t->state == TASK_BLOCKED || t->state == TASK_SLEEPING) {
        t->state = TASK_READY;
        need_resched = true;
    }
}

struct tcb *task_create(const char *name, int (*entry)(void *), void *arg)
{
    uint64_t f = irq_save();
    reap_zombies();
    struct tcb *t = NULL;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED) { t = &tasks[i]; break; }
    if (!t) { irq_restore(f); return NULL; }
    memset(t, 0, sizeof *t);
    t->state = TASK_BLOCKED;            /* not runnable until fully built */
    irq_restore(f);

    t->kstack = kmalloc(KSTACK_SIZE);
    if (!t->kstack) { t->state = TASK_UNUSED; return NULL; }
    t->kstack_size = KSTACK_SIZE;

    /* Fake context_switch frame: r15 r14 r13 r12 rbp rbx rflags rip */
    uint64_t *sp = (uint64_t *)(((uintptr_t)t->kstack + KSTACK_SIZE) & ~0xFull);
    *--sp = (uint64_t)thread_trampoline;    /* return address          */
    *--sp = 0x002;                          /* RFLAGS (IF=0)           */
    *--sp = (uint64_t)entry;                /* rbx -> entry            */
    *--sp = 0;                              /* rbp                     */
    *--sp = (uint64_t)arg;                  /* r12 -> argument         */
    *--sp = 0; *--sp = 0; *--sp = 0;        /* r13 r14 r15             */
    t->rsp = (uint64_t)sp;

    t->pid = next_pid++;
    t->ppid = cur ? cur->pid : 0;
    t->pgid = t->sid = t->pid;
    strlcpy(t->name, name, sizeof t->name);
    t->cr3 = read_cr3();
    t->quantum = SCHED_QUANTUM;
    t->start_tick = pit_ticks();
    t->cwd = cur ? cur->cwd : NULL;
    t->tty = cur ? cur->tty : 0;
    t->umask = 022;
    t->state = TASK_READY;
    kprintf("sched: created pid %d (%s)\n", t->pid, t->name);
    return t;
}

static struct tcb *pick_next(void)
{
    int start = (int)(cur - tasks);
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct tcb *t = &tasks[(start + i) % MAX_TASKS];
        if (t->pid != 0 && t->state == TASK_READY) return t;
    }
    /* nothing else runnable: keep the current thread if it can run, else idle */
    if (cur->state == TASK_RUNNING) return cur;
    return &tasks[0];
}

void schedule(void)
{
    if (!running) return;
    need_resched = false;
    struct tcb *prev = cur;
    struct tcb *next = pick_next();
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->quantum = SCHED_QUANTUM;
    if (next == prev) return;

    next->switches++;
    nswitches++;
    cur = next;
    uint64_t ktop = ((uintptr_t)next->kstack + next->kstack_size) & ~0xFull;
    tss_set_kernel_stack(ktop);
    syscall_kernel_rsp = ktop;
    /* user processes own SSE state (the kernel is built without SSE) and a
     * TLS base; kernel threads never touch either */
    if (prev->user) {
        __asm__ volatile("fxsave %0" : "=m"(prev->fpu));
        prev->fs_base = rdmsr(MSR_FS_BASE);
    }
    if (next->user) {
        __asm__ volatile("fxrstor %0" :: "m"(next->fpu));
        wrmsr(MSR_FS_BASE, next->fs_base);
    }
    if (next->cr3 != prev->cr3) write_cr3(next->cr3);
    context_switch(&prev->rsp, next->rsp);
}

void sched_yield(void)
{
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void sched_tick(void)
{
    if (!running) return;
    uint64_t now = pit_ticks();
    cur->cpu_ticks++;
    if (cur->pid == 0) ticks_idle++;
    else if (cur->user) ticks_user++;
    else ticks_system++;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= now) {
            tasks[i].state = TASK_READY;
            need_resched = true;
        }
    if (cur->pid == 0) { if (need_resched) schedule(); return; }
    if (preempt && (--cur->quantum <= 0 || need_resched)) schedule();
}

void sleep_on(void *chan)
{
    if (!running || cur->pid == 0) {           /* no other thread to run */
        sti(); hlt(); cli();
        return;
    }
    cur->wait_chan = chan;
    cur->state = TASK_BLOCKED;
    schedule();
    cur->wait_chan = NULL;
}

void wakeup(void *chan)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_chan == chan) {
            tasks[i].state = TASK_READY;
            need_resched = true;
        }
}

void task_sleep_ms(uint64_t ms)
{
    uint64_t f = irq_save();
    if (cur->pid == 0) {
        irq_restore(f);
        pit_sleep_ms(ms);
        return;
    }
    cur->wake_tick = pit_ticks() + ms * PIT_HZ / 1000;
    cur->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void task_exit(int code)
{
    cli();
    kprintf("sched: pid %d (%s) exited with status %d\n", cur->pid, cur->name, code);
    /* close now, not at reap time: pipe and socket peers must see EOF */
    for (int fd = 0; fd < MAX_FDS; fd++) if (cur->fds[fd]) { vfs_close(cur->fds[fd]); cur->fds[fd] = NULL; }
    cur->exit_code = code;
    process_exit(cur);                          /* parent, children, vfork */
    cur->state = TASK_ZOMBIE;
    wakeup(cur);                                /* anyone waiting on us */
    schedule();
    panic("zombie task %d was rescheduled", cur->pid);
}

int task_wait(int pid)
{
    uint64_t f = irq_save();
    struct tcb *t = NULL;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) { t = &tasks[i]; break; }
    if (!t || t == cur) { irq_restore(f); return -ECHILD; }
    t->waiters++;
    while (t->state != TASK_ZOMBIE) sleep_on(t);
    int code = t->exit_code;
    t->waiters--;
    irq_restore(f);
    return code;
}

void sched_idle_loop(void)
{
    for (;;) {
        cli();
        reap_zombies();
        bool ready = false;
        for (int i = 1; i < MAX_TASKS; i++) if (tasks[i].state == TASK_READY) { ready = true; break; }
        if (ready) { schedule(); sti(); continue; }
        __asm__ volatile("sti; hlt" ::: "memory");   /* atomic: no lost wakeup */
    }
}

int task_snapshot(struct tcb *out, int max)
{
    int n = 0;
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_TASKS && n < max; i++)
        if (tasks[i].state != TASK_UNUSED) out[n++] = tasks[i];
    irq_restore(f);
    return n;
}
