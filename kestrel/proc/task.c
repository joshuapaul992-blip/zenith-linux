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
#include <kernel/time.h>
#include <kernel/vm.h>

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

/* Kernel stacks: KSTACK_SIZE bytes, identity mapped (drivers may hand stack
 * buffers to DMA), with the page below made a guard page. Running off the
 * end of a stack faults on the guard instead of silently corrupting the
 * neighbouring memory; the double-fault handler reports it. */
#define KSTACK_PAGES (KSTACK_SIZE / PAGE_SIZE)

static uint8_t *kstack_alloc(void)
{
    uint64_t base = pmm_alloc_contig(KSTACK_PAGES + 1);
    if (!base) return NULL;
    if (!vmm_set_guard(base, true)) { pmm_free_contig(base, KSTACK_PAGES + 1); return NULL; }
    return (uint8_t *)(uintptr_t)(base + PAGE_SIZE);
}

static void kstack_free(uint8_t *stack)
{
    uint64_t base = (uint64_t)(uintptr_t)stack - PAGE_SIZE;
    vmm_set_guard(base, false);                         /* an ordinary page again */
    pmm_free_contig(base, KSTACK_PAGES + 1);
}

bool task_stack_guard_hit(const struct tcb *t, uint64_t addr)
{
    if (!t || !t->kstack || t->pid == 0) return false;
    uint64_t guard = (uint64_t)(uintptr_t)t->kstack - PAGE_SIZE;
    return addr >= guard && addr < guard + PAGE_SIZE;
}

static void reap_one(struct tcb *t)
{
    for (int fd = 0; fd < MAX_FDS; fd++) if (t->fds[fd]) vfs_close(t->fds[fd]);
    if (t->mm) { mm_put(t->mm); t->mm = NULL; }       /* normally gone at exit already */
    t->pml4 = 0;
    kstack_free(t->kstack);
    t->kstack = NULL;
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

    t->kstack = kstack_alloc();
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

void task_set_idle_class(struct tcb *t) { t->idle_class = true; }

static bool normal_ready(void)
{
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_READY && !tasks[i].idle_class) return true;
    return false;
}

/* Round robin among ready tasks; idle-class tasks only when no normal task
 * is ready (they are what the CPU does instead of halting). */
static struct tcb *pick_next(void)
{
    int start = (int)(cur - tasks);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 1; i <= MAX_TASKS; i++) {
            struct tcb *t = &tasks[(start + i) % MAX_TASKS];
            if (t->pid != 0 && t->state == TASK_READY && t->idle_class == (pass == 1)) return t;
        }
        /* no other normal task: a normal current task keeps the CPU */
        if (pass == 0 && cur->state == TASK_RUNNING && !cur->idle_class && cur->pid != 0) return cur;
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
    uint64_t ms = time_ms();
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= now) {
            tasks[i].state = TASK_READY;
            need_resched = true;
        }
        if (tasks[i].alarm_at && ms >= tasks[i].alarm_at && tasks[i].state != TASK_ZOMBIE &&
            tasks[i].state != TASK_UNUSED) {          /* ITIMER_REAL expired */
            tasks[i].alarm_at = tasks[i].alarm_every ? ms + tasks[i].alarm_every : 0;
            signal_send(&tasks[i], SIGALRM);
        }
    }
    if (cur->pid == 0) { if (need_resched) schedule(); return; }
    if (cur->idle_class && preempt && normal_ready()) { schedule(); return; }   /* yield at once */
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

void kmutex_lock(struct kmutex *m)
{
    uint64_t f = irq_save();
    if (m->owner == cur) { m->depth++; irq_restore(f); return; }
    while (m->owner) sleep_on(m);
    m->owner = cur;
    m->depth = 1;
    irq_restore(f);
}

void kmutex_unlock(struct kmutex *m)
{
    uint64_t f = irq_save();
    if (m->owner != cur) panic("kmutex_unlock: not the owner (pid %d)", cur->pid);
    if (--m->depth == 0) { m->owner = NULL; wakeup(m); }
    irq_restore(f);
}

bool kmutex_held(const struct kmutex *m) { return m->owner == cur; }

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
    /* Give the address space back now, not when the parent reaps us: memory
     * comes back at once (the OOM killer relies on that). Run on the kernel
     * page tables from here on. */
    if (cur->mm) {
        struct mm *mm = cur->mm;
        cur->mm = NULL;
        cur->pml4 = 0;
        cur->cr3 = uvm_kernel_pml4();
        write_cr3(cur->cr3);
        sti();
        mm_put(mm);
        cli();
    }
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
