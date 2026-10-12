/* proc/task.c -- kernel threads and a preemptive round-robin scheduler.
 *
 * SMP: one run queue (the task table) shared by every CPU; each CPU has its
 * own current task (percpu.current) and idle thread, and runs schedule()
 * itself from its local timer. Run states change under the scheduler lock
 * (sched_lock below), not the big kernel lock: a task sleeps, wakes and
 * switches whether or not it holds the big lock. A task keeps its own big
 * lock depth across a switch -- schedule() lets the lock go for the task
 * leaving and takes it back for the one arriving -- and a task taken off one
 * CPU is not resumed on another until its registers are saved (on_cpu).
 *
 * Every thread owns a 16 KiB kernel stack. A switch saves callee-saved
 * registers on the outgoing stack (context_switch in switch.asm) and
 * resumes the incoming thread wherever it last called schedule() -- which,
 * for a preempted thread, is inside the timer interrupt handler. When the
 * handler returns, iretq restores the thread's full user/kernel state. */
#include <kernel/task.h>
#include <kernel/net.h>
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
#include <kernel/smp.h>
#include <kernel/spinlock.h>

extern void context_switch(uint64_t *save_rsp, uint64_t load_rsp, int *saved);
extern void thread_trampoline(void);
extern uint8_t boot_stack[], boot_stack_top[];

#define MSR_KERNEL_GS_BASE 0xC0000102

static struct tcb tasks[MAX_TASKS];
static struct tcb idle_tasks[MAX_CPUS];            /* the APs' idle threads */
#define cur          (this_task())           /* writes: this_cpu()->current, irqs off */
#define need_resched (this_cpu()->need_resched)
static int next_pid = 1;
static uint32_t zombie_gen;                         /* task_exit: the idle loop reaps */
static bool running, preempt = true;
static uint64_t nswitches;

/* Every task's state, wait_chan, wake_ms and sched_cur change under this.
 * Taken inside the big kernel lock when both are held, never the other way
 * round; nothing sleeps, faults or waits for another CPU while holding it. */
static struct spinlock sched_lock;

/* A sleeping t can run now (sched_lock held): mark it, and get an idle CPU
 * (if any) to look. A task woken before its own CPU has switched it out
 * simply stays on that CPU. */
static void make_ready_locked(struct tcb *t)
{
    if (t->state != TASK_BLOCKED && t->state != TASK_SLEEPING) return;
    need_resched = true;                                /* this CPU's */
    if (t->sched_cur) { t->state = TASK_RUNNING; return; }
    t->state = TASK_READY;
    smp_kick_idle();
}

static void make_ready(struct tcb *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    make_ready_locked(t);
    spin_unlock_irqrestore(&sched_lock, f);
}

/* The current task stops running at its next schedule(): asleep on chan
 * (NULL: none) until a wakeup, or until uptime wake_ms when that is set. */
static void block_self(void *chan, uint64_t wake_ms)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    cur->wait_chan = chan;
    cur->wake_ms = wake_ms;
    cur->state = wake_ms ? TASK_SLEEPING : TASK_BLOCKED;
    spin_unlock_irqrestore(&sched_lock, f);
}

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

/* ---- shared process state ------------------------------------------------- */
struct files_struct *files_new(void)
{
    struct files_struct *f = kzalloc(sizeof *f);
    if (f) f->refs = 1;
    return f;
}

struct files_struct *files_dup(struct files_struct *src)
{
    struct files_struct *f = files_new();
    if (!f) return NULL;
    uint64_t fl = spin_lock_irqsave(&src->lock);
    for (int i = 0; i < MAX_FDS; i++) {
        f->fd[i] = vfs_file_get(src->fd[i]);
        f->cloexec[i] = src->cloexec[i];
    }
    spin_unlock_irqrestore(&src->lock, fl);
    return f;
}

void files_put(struct files_struct *f)
{
    if (!f || __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (int i = 0; i < MAX_FDS; i++)
        if (f->fd[i]) { struct file *x = f->fd[i]; f->fd[i] = NULL; vfs_locks_fd_closed(x); vfs_close(x); }
    kfree(f);
}

struct fs_struct *fs_new(struct vnode *cwd, uint32_t umask)
{
    struct fs_struct *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->refs = 1;
    f->cwd = cwd;
    f->umask = umask;
    return f;
}

void fs_put(struct fs_struct *f)
{
    if (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(f);
}

struct sighand_struct *sighand_new(const struct sighand_struct *copy)
{
    struct sighand_struct *h = kzalloc(sizeof *h);
    if (!h) return NULL;
    if (copy) memcpy(h->action, copy->action, sizeof h->action);
    h->refs = 1;
    return h;
}

void sighand_put(struct sighand_struct *h)
{
    if (h && __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(h);
}

struct signal_struct *signal_new(void)
{
    struct signal_struct *g = kzalloc(sizeof *g);
    if (!g) return NULL;
    g->refs = 1;
    g->nr_threads = 1;
    return g;
}

void signal_put(struct signal_struct *g)
{
    if (g && __atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(g);
}

/* Fresh per-process state for a new task (a kernel thread, or the start of
 * a fork before the clone flags decide what is shared). */
static bool task_state_new(struct tcb *t, struct tcb *from)
{
    t->files = files_new();
    t->fs = fs_new(from && from->fs ? from->fs->cwd : NULL, from && from->fs ? from->fs->umask : 022);
    t->sighand = sighand_new(NULL);
    t->signal = signal_new();
    if (t->files && t->fs && t->sighand && t->signal) return true;
    files_put(t->files); fs_put(t->fs); sighand_put(t->sighand); signal_put(t->signal);
    t->files = NULL; t->fs = NULL; t->sighand = NULL; t->signal = NULL;
    return false;
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
    if (!task_state_new(t, NULL)) panic("sched: out of memory");
    t->bkl_depth = 1;
    t->sched_cur = true;
    t->on_cpu = 1;
    this_cpu()->current = t;
    this_cpu()->idle = t;
    tss_set_kernel_stack((uint64_t)boot_stack_top);
    this_cpu()->kernel_rsp = (uint64_t)boot_stack_top;
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
    files_put(t->files); t->files = NULL;              /* normally dropped at exit already */
    fs_put(t->fs); t->fs = NULL;
    sighand_put(t->sighand); t->sighand = NULL;
    signal_put(t->signal); t->signal = NULL;
    if (t->mm) { mm_put(t->mm); t->mm = NULL; }       /* normally gone at exit already */
    t->pml4 = 0;
    kstack_free(t->kstack);
    t->kstack = NULL;
    t->state = TASK_UNUSED;
}

/* Still on its stack: exiting on another CPU, or not yet switched out. */
static bool on_a_cpu(struct tcb *t)
{
    return __atomic_load_n(&t->sched_cur, __ATOMIC_ACQUIRE) || __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE);
}

/* Zombies go away here unless someone still wants their exit status: a
 * task_wait() caller, or a parent process that has not called wait4(). */
static void reap_zombies(void)
{
    for (int i = 1; i < MAX_TASKS; i++) {
        struct tcb *t = &tasks[i];
        if (t->state != TASK_ZOMBIE || t == cur || t->waiters || process_zombie_kept(t) || on_a_cpu(t)) continue;
        reap_one(t);
    }
}

void task_reap(struct tcb *t)
{
    uint64_t f = irq_save();
    if (t->state == TASK_ZOMBIE && t != cur && !t->waiters && !on_a_cpu(t)) reap_one(t);
    irq_restore(f);
}

struct tcb *task_find(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) return &tasks[i];
    return NULL;
}

struct tcb *task_slot(int i) { return (i >= 0 && i < MAX_TASKS) ? &tasks[i] : NULL; }

void task_interrupt(struct tcb *t) { make_ready(t); }

/* The idle thread of an application processor, on its start-up stack. */
struct tcb *sched_cpu_idle_task(int cpu, uint8_t *stack, size_t size)
{
    struct tcb *t = &idle_tasks[cpu];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "idle/%d", cpu);
    t->state = TASK_RUNNING;
    t->kstack = stack;
    t->kstack_size = size;
    t->cr3 = read_cr3();
    t->quantum = SCHED_QUANTUM;
    t->bkl_depth = 1;
    t->sched_cur = true;
    t->on_cpu = 1;
    if (!task_state_new(t, NULL)) return NULL;
    return t;
}

void task_block_self(void *chan, uint64_t deadline_ms)
{
    uint64_t wake = 0;
    if (deadline_ms) {
        uint64_t now = time_ms();
        wake = uptime_ms() + (deadline_ms > now ? deadline_ms - now : 0);
        if (!wake) wake = 1;
    }
    block_self(chan, wake);
}

void task_wake(struct tcb *t) { make_ready(t); }

/* A new thread, not yet runnable: the creator finishes it (clone fills in
 * the address space, TLS, ...) and then calls task_start(). Nothing else
 * stops another CPU from switching to it, and a switch reads those fields. */
struct tcb *task_create_paused(const char *name, int (*entry)(void *), void *arg)
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
    if (!task_state_new(t, cur)) { kstack_free(t->kstack); t->kstack = NULL; t->state = TASK_UNUSED; return NULL; }
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

    t->pid = t->tgid = next_pid++;
    t->ppid = cur ? cur->tgid : 0;
    t->pgid = t->sid = t->pid;
    strlcpy(t->name, name, sizeof t->name);
    t->cr3 = read_cr3();
    t->quantum = SCHED_QUANTUM;
    t->start_tick = uptime_ms() * PIT_HZ / 1000;
    t->tty = cur ? cur->tty : 0;
    t->bkl_depth = 1;                   /* thread_trampoline takes the lock first */
    kprintf("sched: created pid %d (%s)\n", t->pid, t->name);
    return t;
}

void task_start(struct tcb *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->state = TASK_READY;              /* no preemption of the creator: an idle CPU may take it */
    smp_kick_idle();
    spin_unlock_irqrestore(&sched_lock, f);
}

struct tcb *task_create(const char *name, int (*entry)(void *), void *arg)
{
    struct tcb *t = task_create_paused(name, entry, arg);
    if (t) task_start(t);
    return t;
}

void task_set_idle_class(struct tcb *t) { t->idle_class = true; }

static bool normal_ready(void)
{
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_READY && !tasks[i].idle_class) return true;
    return false;
}

/* Until it becomes CPU 0's idle loop, the boot thread is an ordinary
 * thread that can be preempted holding a lock (a TTY, while it prints):
 * it must then get the CPU back like any other, or threads yielding to the
 * lock's holder would never let it run. Cleared by sched_idle_loop(). */
static bool boot_thread_busy = true;

/* Round robin among ready tasks; idle-class tasks only when no normal task
 * is ready (they are what the CPU does instead of halting). */
static struct tcb *pick_next(void)
{
    static int rr;                                      /* idle threads are not in tasks[] */
    int start = (cur >= tasks && cur < tasks + MAX_TASKS) ? (int)(cur - tasks) : rr++ % MAX_TASKS;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 1; i <= MAX_TASKS; i++) {
            struct tcb *t = &tasks[(start + i) % MAX_TASKS];
            bool eligible = t->pid != 0 || (boot_thread_busy && this_cpu()->id == 0);
            if (eligible && t->state == TASK_READY && t->idle_class == (pass == 1)) return t;
        }
        /* no other normal task: a normal current task keeps the CPU */
        if (pass == 0 && cur->state == TASK_RUNNING && !cur->idle_class && cur->pid != 0) return cur;
    }
    /* nothing else runnable: keep the current thread if it can run, else idle */
    if (cur->state == TASK_RUNNING) return cur;
    return this_cpu()->idle;
}

/* The arriving task's side of a switch, on its own stack: its big kernel
 * lock depth back, taking the lock if the task leaving did not hand it over
 * (also run by thread_trampoline for a new task). */
void sched_switch_tail(void)
{
    struct percpu *c = this_cpu();
    int d = c->current->bkl_depth;
    if (d && !c->bkl_depth) bkl_enter();
    c->bkl_depth = d;
    if (!d) bkl_run_deferred();                 /* interrupts deferred as the lock left the last task */
}

void schedule(void)
{
    if (!running) return;
    struct tcb *prev = cur;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    need_resched = false;
    struct tcb *next = pick_next();
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->quantum = SCHED_QUANTUM;
    if (next == prev) { spin_unlock_irqrestore(&sched_lock, f); return; }
    prev->sched_cur = false;
    next->sched_cur = true;
    next->switches++;
    nswitches++;
    spin_unlock_irqrestore(&sched_lock, f);

    /* next may still be leaving another CPU: wait until its registers are
     * saved (a few instructions away; nothing there waits for us) */
    while (__atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) { tlb_service(); cpu_relax(); }
    next->on_cpu = 1;
    struct percpu *c = this_cpu();
    c->current = next;
    /* Each task its own nesting. From one holder of the big lock to another
     * the lock stays with this CPU, as it always did; a task arriving
     * without it lets it go, one arriving with it after a task that did not
     * hold it queues for it in sched_switch_tail. */
    prev->bkl_depth = c->bkl_depth;
    if (c->bkl_depth && !next->bkl_depth) bkl_exit_switch();
    uint64_t ktop = ((uintptr_t)next->kstack + next->kstack_size) & ~0xFull;
    tss_set_kernel_stack(ktop);
    c->kernel_rsp = ktop;
    /* user processes own SSE state (the kernel is built without SSE), a TLS
     * base (FS) and a GS base (in KERNEL_GS_BASE while in the kernel);
     * kernel threads never touch them */
    if (prev->user) {
        __asm__ volatile("fxsave %0" : "=m"(prev->fpu));
        prev->fs_base = rdmsr(MSR_FS_BASE);
        prev->gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    }
    if (next->user) {
        __asm__ volatile("fxrstor %0" :: "m"(next->fpu));
        wrmsr(MSR_FS_BASE, next->fs_base);
        wrmsr(MSR_KERNEL_GS_BASE, next->gs_base);
    }
    if (next->cr3 != prev->cr3) cpu_load_cr3(next->cr3);
    kstat_switch(prev, next);
    context_switch(&prev->rsp, next->rsp, &prev->on_cpu);
    sched_switch_tail();
}

/* Give the CPU, and the big kernel lock, to whoever wants them: a task
 * waiting for the lock on another CPU gets it before this one has it back
 * (a busy-wait that yields relies on that to let the holder finish). */
void sched_yield(void)
{
    uint64_t f = irq_save();
    int d = bkl_release_all();
    schedule();
    bkl_reacquire(d);
    irq_restore(f);
}

/* Every CPU, every tick: time accounting and time slices. */
void sched_tick_local(void)
{
    if (!running) return;
    cur->cpu_ticks++;
    if (cur->pid == 0) __atomic_add_fetch(&ticks_idle, 1, __ATOMIC_RELAXED);
    else if (cur->user) __atomic_add_fetch(&ticks_user, 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&ticks_system, 1, __ATOMIC_RELAXED);
    if (cur->pid == 0) { if (need_resched) schedule(); return; }
    if (cur->idle_class && preempt && normal_ready()) { schedule(); return; }   /* yield at once */
    if (preempt && (--cur->quantum <= 0 || need_resched)) schedule();
}

/* Every other CPU's tick, without the kernel lock (idt.c): the same
 * accounting and time slice as sched_tick_local(). The scheduler has its
 * own lock, so a slice that runs out switches tasks right here, and a CPU
 * running user code or halted never queues for the big lock on a tick. */
void sched_tick_fast(void)
{
    if (!running) return;
    struct tcb *t = cur;
    t->cpu_ticks++;
    if (t->pid == 0) { __atomic_add_fetch(&ticks_idle, 1, __ATOMIC_RELAXED); return; }   /* the idle loop looks itself */
    if (t->user) __atomic_add_fetch(&ticks_user, 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&ticks_system, 1, __ATOMIC_RELAXED);
    if (!preempt) return;
    if ((t->idle_class && normal_ready()) || --t->quantum <= 0 || need_resched) schedule();
}

/* The earliest ITIMER_REAL of any process (time_ms; 0: none). It may be
 * early -- a timer since cancelled -- never late: the locked tick then
 * looks at every process and sets it again. */
static uint64_t alarm_next;

void sched_alarm_armed(uint64_t at)             /* under the kernel lock */
{
    if (at && (!alarm_next || at < alarm_next)) alarm_next = at;
}

static void wake_sleepers(void)
{
    uint64_t now = uptime_ms();
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_ms <= now) make_ready_locked(&tasks[i]);
    spin_unlock_irqrestore(&sched_lock, f);
}

/* The boot CPU's tick: timed sleeps and interval timers, then its slice. */
void sched_tick(void)
{
    if (!running) return;
    wake_sleepers();
    sched_alarms();
    net_tick();                                 /* received frames or a due timer wake netd */
    sched_tick_local();
}

/* ITIMER_REAL timers that are due (big lock held). */
void sched_alarms(void)
{
    if (!running) return;
    uint64_t ms = time_ms();
    uint64_t next = 0;
    for (int i = 1; i < MAX_TASKS; i++) {
        struct signal_struct *g = tasks[i].signal;      /* ITIMER_REAL: per process, on its leader */
        if (!g || !g->alarm_at || tasks[i].pid != tasks[i].tgid ||
            tasks[i].state == TASK_ZOMBIE || tasks[i].state == TASK_UNUSED) continue;
        if (ms >= g->alarm_at) {
            g->alarm_at = g->alarm_every ? ms + g->alarm_every : 0;
            signal_send(&tasks[i], SIGALRM);
        }
        if (g->alarm_at && (!next || g->alarm_at < next)) next = g->alarm_at;
    }
    alarm_next = next;
}

/* The boot CPU's tick without the kernel lock (idt.c), when no interval
 * timer is due (sched_tick_needs_lock): sleepers, the network daemon's
 * timer, the slice. */
bool sched_tick_needs_lock(void)
{
    uint64_t a = __atomic_load_n(&alarm_next, __ATOMIC_RELAXED);
    return a && time_ms() >= a;
}

void sched_tick_boot_fast(void)
{
    if (!running) return;
    wake_sleepers();
    net_tick();
    sched_tick_fast();
}

void sleep_on(void *chan)
{
    if (!running || cur->pid == 0) {           /* no other thread to run */
        sti(); hlt(); cli();
        return;
    }
    block_self(chan, 0);
    schedule();
    cur->wait_chan = NULL;
}

void sleep_on_locked(void *chan, struct spinlock *l)
{
    if (!running || cur->pid == 0) {
        spin_unlock_keep_irqs(l);
        sti(); hlt(); cli();
        spin_lock_irqsave(l);
        return;
    }
    block_self(chan, 0);
    spin_unlock_keep_irqs(l);
    schedule();
    cur->wait_chan = NULL;
    spin_lock_irqsave(l);                       /* interrupts are off already */
}

void sleep_on_until(void *chan, uint64_t deadline_ms)
{
    if (!running || cur->pid == 0) { sti(); hlt(); cli(); return; }
    task_block_self(chan, deadline_ms);
    schedule();
    cur->wait_chan = NULL;
}

/* Mutexes are taken with and without the big kernel lock (an address
 * space's, by mmap and by faults): one spinlock makes taking one atomic. */
static struct spinlock kmutex_spin;

void kmutex_lock(struct kmutex *m)
{
    uint64_t f = spin_lock_irqsave(&kmutex_spin);
    if (m->owner == cur) { m->depth++; spin_unlock_irqrestore(&kmutex_spin, f); return; }
    while (m->owner) sleep_on_locked(m, &kmutex_spin);
    __atomic_store_n(&m->owner, cur, __ATOMIC_RELEASE);
    m->depth = 1;
    spin_unlock_irqrestore(&kmutex_spin, f);
}

void kmutex_unlock(struct kmutex *m)
{
    uint64_t f = spin_lock_irqsave(&kmutex_spin);
    if (m->owner != cur) panic("kmutex_unlock: not the owner (pid %d)", cur->pid);
    if (--m->depth == 0) {
        __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
        wakeup_waiters(m);                      /* nobody polls a mutex */
    }
    spin_unlock_irqrestore(&kmutex_spin, f);
}

bool kmutex_held(const struct kmutex *m) { return m->owner == cur; }

static uint64_t poll_seq;                       /* advanced by every wakeup() */
static int      pollers;                        /* tasks in poll_sleep        */

/* A sleeping poller cares about chan (task.h: struct poll_table). */
static bool polls_for(const struct tcb *t, void *chan)
{
    const struct poll_table *pt = t->poll_tab;
    if (!pt || pt->all) return true;
    for (int i = 0; i < pt->n; i++) if (pt->chan[i] == chan) return true;
    return false;
}

void wakeup(void *chan)
{
    __atomic_add_fetch(&poll_seq, 1, __ATOMIC_SEQ_CST);
    bool polls = __atomic_load_n(&pollers, __ATOMIC_SEQ_CST) > 0;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = &tasks[i];
        if (t->state != TASK_BLOCKED && t->state != TASK_SLEEPING) continue;
        if (t->wait_chan == chan || (polls && t->wait_chan == &poll_seq && polls_for(t, chan))) make_ready_locked(t);
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void wakeup_waiters(void *chan)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = &tasks[i];
        if ((t->state == TASK_BLOCKED || t->state == TASK_SLEEPING) && t->wait_chan == chan) make_ready_locked(t);
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void io_pause(void *chan, uint32_t us)
{
    if (!running || cur->pid == 0 || !irqs_enabled()) { udelay(us); return; }
    uint64_t f = irq_save();
    task_block_self(chan, time_ms() + 1);       /* the next tick at the latest */
    schedule();
    cur->wait_chan = NULL;
    irq_restore(f);
}

void io_wake(void *chan) { wakeup_waiters(chan); }

uint64_t poll_seq_read(void) { return __atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST); }

void poll_begin(struct poll_table *pt)
{
    pt->all = false;
    pt->n = 0;
    pt->calls = 0;
    cur->poll_tab = pt;
}

void poll_wait(void *chan)
{
    struct poll_table *pt = cur->poll_tab;
    if (!pt) return;                            /* a poll op called outside a scan */
    pt->calls++;
    if (pt->all) return;
    for (int i = 0; i < pt->n; i++) if (pt->chan[i] == chan) return;
    if (pt->n == POLL_CHANS) { pt->all = true; return; }
    pt->chan[pt->n++] = chan;
}

void poll_end(void) { cur->poll_tab = NULL; }

/* The sequence number is checked with sched_lock held, as the task blocks:
 * wakeup() advances it before it takes the lock to look for sleepers, so
 * a wakeup either moved it already (no sleep) or finds this task asleep.
 * Pollers run without the kernel lock, so nothing else orders the two. */
void poll_sleep(uint64_t seq, uint64_t deadline_ms, struct poll_table *pt)
{
    uint64_t f = irq_save();
    if (__atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST) == seq && !signal_pending()) {
        /* Objects that change state without a wakeup() (a device polled by
         * a driver) are still seen within 100 ms; with every object's
         * channels known, a second is plenty. */
        uint64_t now = time_ms(), cap = now + (pt && !pt->all ? 1000 : 100);
        uint64_t d = deadline_ms && deadline_ms < cap ? deadline_ms : cap;
        uint64_t wake = uptime_ms() + (d > now ? d - now : 0);
        __atomic_add_fetch(&pollers, 1, __ATOMIC_SEQ_CST);
        cur->poll_tab = pt;
        spin_lock_irqsave(&sched_lock);
        bool sleep = __atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST) == seq;
        if (sleep) {
            cur->wait_chan = &poll_seq;
            cur->wake_ms = wake ? wake : 1;
            cur->state = TASK_SLEEPING;
        }
        spin_unlock_keep_irqs(&sched_lock);
        if (sleep) schedule();
        cur->wait_chan = NULL;
        cur->poll_tab = NULL;
        __atomic_sub_fetch(&pollers, 1, __ATOMIC_SEQ_CST);
    }
    irq_restore(f);
}

void task_sleep_ms(uint64_t ms)
{
    uint64_t f = irq_save();
    if (cur->pid == 0) {
        irq_restore(f);
        pit_sleep_ms(ms);
        return;
    }
    block_self(NULL, uptime_ms() + ms);
    schedule();
    irq_restore(f);
}

void task_exit(int code)
{
    cur->exit_code = code;
    /* User threads: clear_child_tid + futex wake, robust futexes -- while
     * the address space is still there (this touches user memory). */
    if (cur->user) { sti(); process_thread_exit(cur); }
    cli();
    kstat_thread_exit(cur);
    kprintf("sched: pid %d (%s) exited with status %d\n", cur->pid, cur->name, code);
    /* close now, not at reap time: pipe and socket peers must see EOF (the
     * table goes when the last thread sharing it is gone) */
    struct files_struct *files = cur->files;
    cur->files = NULL;
    files_put(files);
    /* Give the address space back now, not when the parent reaps us: memory
     * comes back at once (the OOM killer relies on that). Run on the kernel
     * page tables from here on. */
    if (cur->mm) {
        struct mm *mm = cur->mm;
        cur->mm = NULL;
        cur->pml4 = 0;
        cur->cr3 = uvm_kernel_pml4();
        cpu_load_cr3(cur->cr3);
        sti();
        mm_put(mm);
        cli();
    }
    process_exit(cur);                          /* thread group, parent, children, vfork */
    uint64_t f = spin_lock_irqsave(&sched_lock);
    cur->state = TASK_ZOMBIE;
    spin_unlock_irqrestore(&sched_lock, f);
    __atomic_add_fetch(&zombie_gen, 1, __ATOMIC_RELEASE);
    wakeup(cur);                                /* anyone waiting on us */
    schedule();
    panic("zombie task %d was rescheduled", cur->pid);
}

/* Wait for process (or kernel thread) `pid` to end: every thread of it. */
int task_wait(int pid)
{
    uint64_t f = irq_save();
    struct tcb *t = NULL;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) { t = &tasks[i]; break; }
    if (!t || t == cur) { irq_restore(f); return -ECHILD; }
    t->waiters++;
    for (;;) {
        if (t->pid != pid) {                    /* a thread's execve took over the pid: */
            t = task_find(pid);                 /* the waiter count moved with it        */
            if (!t) { irq_restore(f); return -ECHILD; }
        }
        if (t->state == TASK_ZOMBIE && (!t->signal || t->signal->nr_threads == 0)) break;
        sleep_on(t);
    }
    int code = t->exit_code;
    t->waiters--;
    irq_restore(f);
    return code;
}

static bool any_ready(void)
{
    for (int i = 1; i < MAX_TASKS; i++)
        if (__atomic_load_n(&tasks[i].state, __ATOMIC_RELAXED) == TASK_READY) return true;
    return false;
}

/* A CPU's idle thread. Entered holding the big kernel lock, which it lets
 * go of at once: it looks for work and switches to it without the lock
 * (the scheduler has its own), and takes it only to reap the threads that
 * exited since it last looked. A task that held the lock when it was
 * switched out queues for it as it comes back (sched_switch_tail). */
void sched_idle_loop(void)
{
    struct percpu *c = this_cpu();
    uint32_t reaped = 0;
    if (c->id == 0) boot_thread_busy = false;
    cli();
    bkl_release_all();
    for (;;) {
        cli();
        uint32_t g = __atomic_load_n(&zombie_gen, __ATOMIC_ACQUIRE);
        if (g != reaped) { bkl_enter(); reap_zombies(); bkl_exit(); reaped = g; }
        if (any_ready()) { schedule(); continue; }
        for (;;) {
            /* in_idle before the look at the task table, make_ready() the
             * other way round (smp_kick_idle): one of us sees the other */
            __atomic_store_n(&c->in_idle, true, __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (any_ready() || __atomic_load_n(&zombie_gen, __ATOMIC_RELAXED) != reaped) break;
            c->ks.idle_tsc = rdtsc();
            __asm__ volatile("sti; hlt" ::: "memory");   /* atomic: no lost wakeup */
            cli();
            if (c->ks.idle_tsc) { c->ks.idle_cyc += rdtsc() - c->ks.idle_tsc; c->ks.idle_tsc = 0; }
        }
        __atomic_store_n(&c->in_idle, false, __ATOMIC_RELEASE);
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
