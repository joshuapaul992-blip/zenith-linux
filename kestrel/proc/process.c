/* proc/process.c -- fork/vfork/clone, execve, wait4, sessions and signals
 *
 * See include/kernel/process.h for the model. Everything here runs in the
 * context of the calling process, with its address space loaded, so user
 * memory is reached directly once it has been checked to be mapped. */
#include <kernel/process.h>
#include <kernel/task.h>
#include <kernel/exec.h>
#include <kernel/uvm.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/arch.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/posix.h>
#include <kernel/uaccess.h>
#include <kernel/vm.h>
#include <kernel/futex.h>

extern void user_return(struct int_frame *f) __attribute__((noreturn));

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_PTRACE         0x00002000
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_DETACHED       0x00400000
#define CLONE_UNTRACED       0x00800000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_IO             0x80000000u
/* Namespaces, pidfds, cgroups are not supported: they fail with EINVAL. */
#define CLONE_SUPPORTED      (0xff | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_PTRACE | \
                              CLONE_VFORK | CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | \
                              CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_UNTRACED | \
                              CLONE_CHILD_SETTID | CLONE_IO)

#define WNOHANG     1

#define EXEC_MAX_STRINGS    1024            /* argv + envp entries        */
#define EXEC_MAX_BYTES      (128u << 10)    /* their total size            */

static uint64_t sigbit(int sig) { return 1ull << (sig - 1); }
#define UNBLOCKABLE (sigbit(SIGKILL) | sigbit(SIGSTOP))

/* ---- user memory (always through uaccess.h) -------------------------------- */
/* A kernel copy of the user string at p, at most max bytes with the NUL.
 * Most strings are short: try a small buffer before a big one. */
static char *ustrdup(uint64_t p, size_t max, int64_t *err)
{
    if (!max) { *err = -E2BIG; return NULL; }
    for (size_t cap = max < 256 ? max : 256;; cap = max) {
        char *buf = kmalloc(cap);
        if (!buf) { *err = -ENOMEM; return NULL; }
        long n = strncpy_from_user(buf, (const char *)p, cap);
        if (n >= 0) {
            char *s = krealloc_shrink(buf, (size_t)n + 1);
            return s;
        }
        kfree(buf);
        if (n != -ENAMETOOLONG || cap == max) { *err = n == -ENAMETOOLONG ? -E2BIG : n; return NULL; }
    }
}

/* ---- process tree ----------------------------------------------------------
 * A process is a thread group. Its leader's tcb (pid == tgid) stands for the
 * process: it stays (a zombie if need be) until every thread has exited and
 * the parent has collected the status. Other threads are reaped as soon as
 * they exit. A process is alive while any of its threads is. */
static bool thread_alive(const struct tcb *t) { return t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE; }
static bool is_leader(const struct tcb *t) { return t->pid == t->tgid; }
static bool proc_alive(const struct tcb *t)
{
    return t && t->state != TASK_UNUSED && t->signal && t->signal->nr_threads > 0;
}
static bool same_group(const struct tcb *a, const struct tcb *b) { return a->signal == b->signal; }

/* The leader of process `pid` (any of its thread ids also finds it). */
static struct tcb *process_of(int pid)
{
    struct tcb *t = task_find(pid);
    if (!t || is_leader(t)) return t;
    return task_find(t->tgid);
}

bool process_zombie_kept(const struct tcb *t)
{
    if (!t->user || !is_leader(t)) return false;        /* threads: reaped at once */
    if (t->signal && t->signal->nr_threads > 0) return true;    /* its threads still run */
    struct tcb *p = task_find(t->ppid);
    if (!proc_alive(p) || !p->user) return false;
    const struct k_sigaction *k = &p->sighand->action[SIGCHLD - 1];
    return !(k->handler == SIG_IGN || (k->flags & SA_NOCLDWAIT));
}

static void vfork_release(struct tcb *t)
{
    if (t->vfork_parent) {
        t->vfork_parent = 0;
        wakeup(&t->vfork_parent);
    }
}

/* Send a thread-directed signal to every other live thread of t's group. */
static void kill_other_threads(struct tcb *t)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *o = task_slot(i);
        if (o != t && thread_alive(o) && same_group(o, t)) signal_send_thread(o, SIGKILL, NULL);
    }
}

/* The calling thread is leaving (interrupts on, its address space still
 * there): the user-memory side of a thread exit. */
void process_thread_exit(struct tcb *t)
{
    if (!t->mm) return;
    if (t->robust_list) {                               /* locks it still holds: owner died */
        uint64_t head = t->robust_list;
        t->robust_list = 0;
        futex_exit_robust(head, t->pid);
    }
    if (t->tid_address) {                               /* CLONE_CHILD_CLEARTID: pthread_join */
        uint64_t a = t->tid_address;
        t->tid_address = 0;
        if (t->mm->users > 1 && !put_user_u32((void *)a, 0)) futex_wake(a, 1, true);
    }
}

/* t is exiting (interrupts off, files and memory already gone). */
void process_exit(struct tcb *t)
{
    vfork_release(t);
    struct signal_struct *g = t->signal;
    g->nr_threads--;
    wakeup(&g->exit_wait);                              /* execve waiting for the others */
    if (g->nr_threads > 0) return;

    /* The last thread: the process is gone. */
    for (int i = 0; i < MAX_TASKS; i++) {               /* orphans: reaped by the kernel */
        struct tcb *c = task_slot(i);
        if (c->state != TASK_UNUSED && c->pid && c->ppid == t->tgid && !same_group(c, t)) c->ppid = 0;
    }
    struct tcb *lead = is_leader(t) ? t : task_find(t->tgid);
    if (!lead) return;
    if (g->group_exit) {                                /* exit_group() or a fatal signal */
        lead->exit_code = g->group_exit_code;
        lead->term_signal = g->group_term_signal;
    }
    wakeup(lead);                                       /* task_wait() */
    if (!t->user) return;
    struct tcb *p = task_find(lead->ppid);
    if (proc_alive(p) && p->user) {
        if (lead->exit_signal) {
            struct ksig_info ci = { .code = lead->term_signal ? CLD_KILLED : CLD_EXITED, .pid = lead->pid,
                                    .uid = lead->uid,
                                    .status = lead->term_signal ? lead->term_signal : (lead->exit_code & 0xff) };
            signal_send_info(p, lead->exit_signal, &ci);
        }
        wakeup(p->signal);                              /* wait4() sleeps on its process */
    }
}

/* exit_group(): end every thread of the process with this status. */
__attribute__((noreturn)) void proc_exit_group(int code)
{
    struct tcb *t = current_task();
    struct signal_struct *g = t->signal;
    uint64_t fl = irq_save();
    if (!g->group_exit) {                               /* a fatal signal may have come first */
        g->group_exit = true;
        g->group_exit_code = code & 0xff;
        g->group_term_signal = 0;
        kill_other_threads(t);
    }
    irq_restore(fl);
    task_exit(code & 0xff);
}

/* ---- fork / vfork / clone ------------------------------------------------- */
static int fork_child_entry(void *arg)
{
    struct int_frame f = *(struct int_frame *)arg;
    kfree(arg);
    /* CLONE_CHILD_SETTID: the tid goes into the child's own memory (a fork
     * child's copy, or the shared memory of CLONE_VM). */
    if (f.vector) put_user_u32((void *)f.vector, (uint32_t)current_task()->pid);
    f.vector = 0x80;
    cli();
    user_return(&f);                    /* FPU, FS base and CR3 set by the scheduler */
}

int64_t proc_clone(uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    struct tcb *p = current_task();
    if (!p->user || !p->uframe) return -EINVAL;
    if (flags & ~(uint64_t)CLONE_SUPPORTED) return -EINVAL;
    if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) return -EINVAL;     /* as Linux */
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) return -EINVAL;
    if ((flags & CLONE_PARENT) && !p->ppid) return -EINVAL;
    if ((flags & CLONE_PARENT_SETTID) && !user_range_ok(ptid, 4)) return -EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !user_range_ok(ctid, 4)) return -EFAULT;
    if ((flags & CLONE_SETTLS) && tls && !uvm_range_ok(tls, 1)) return -EINVAL;
    if (newsp && !user_range_ok(newsp, 0)) return -EINVAL;
    bool thread = flags & CLONE_THREAD;
    if (p->signal->group_exit) return -EAGAIN;          /* the process is dying */

    /* Everything that can fail, before the child exists. */
    struct mm *mm = NULL;
    struct files_struct *files = NULL;
    struct fs_struct *fs = NULL;
    struct sighand_struct *sh = NULL;
    struct int_frame *cf = NULL;
    if (flags & CLONE_VM) { mm = p->mm; mm_get(mm); }
    else if (!(mm = mm_fork(p->mm))) return -ENOMEM;    /* copy-on-write */
    if (flags & CLONE_FILES) { files = p->files; __atomic_add_fetch(&files->refs, 1, __ATOMIC_ACQ_REL); }
    else files = files_dup(p->files);
    if (flags & CLONE_FS) { fs = p->fs; __atomic_add_fetch(&fs->refs, 1, __ATOMIC_ACQ_REL); }
    else fs = fs_new(p->fs->cwd, p->fs->umask);
    if (flags & CLONE_SIGHAND) { sh = p->sighand; __atomic_add_fetch(&sh->refs, 1, __ATOMIC_ACQ_REL); }
    else sh = sighand_new(p->sighand);
    cf = kmalloc(sizeof *cf);
    if (!files || !fs || !sh || !cf) goto nomem;
    *cf = *p->uframe;
    cf->rax = 0;                                        /* the child's return value */
    if (newsp) cf->rsp = newsp;
    cf->vector = (flags & CLONE_CHILD_SETTID) ? ctid : 0;   /* fork_child_entry writes the tid there */

    uint64_t fl = irq_save();                           /* the child must not run half-built */
    struct tcb *c = task_create(p->name, fork_child_entry, cf);
    if (!c) { irq_restore(fl); kfree(cf); cf = NULL; mm_put(mm); mm = NULL; goto again; }
    files_put(c->files); c->files = files;
    fs_put(c->fs); c->fs = fs;
    sighand_put(c->sighand); c->sighand = sh;
    if (thread) {                                       /* same process: share the group */
        signal_put(c->signal);
        c->signal = p->signal;
        __atomic_add_fetch(&p->signal->refs, 1, __ATOMIC_ACQ_REL);
        p->signal->nr_threads++;
        c->tgid = p->tgid;
        c->ppid = p->ppid;
        c->exit_signal = 0;                             /* only the process notifies its parent */
    } else {
        c->ppid = (flags & CLONE_PARENT) ? p->ppid : p->tgid;
        c->exit_signal = (int)(flags & 0xff);
    }
    c->pgid = p->pgid;
    c->sid = p->sid;
    c->user = true;
    c->mm = mm;
    c->pml4 = c->cr3 = mm->pml4;
    c->fs_base = (flags & CLONE_SETTLS) ? tls : rdmsr(MSR_FS_BASE);   /* each thread: its own TLS block */
    c->tid_address = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    c->robust_list = 0;
    __asm__ volatile("fxsave %0" : "=m"(c->fpu));       /* the parent's live SSE state */
    c->uid = p->uid;
    c->gid = p->gid;
    c->tty = p->tty;
    c->ctty = p->ctty;
    memcpy(c->exe, p->exe, sizeof c->exe);
    memcpy(c->cmdline, p->cmdline, sizeof c->cmdline);
    c->cmdline_len = p->cmdline_len;
    c->sig_mask = p->sig_mask;
    c->sig_pending = 0;
    if (flags & CLONE_VFORK) c->vfork_parent = p->pid;
    int pid = c->pid;
    if (flags & CLONE_PARENT_SETTID) put_user_u32((void *)ptid, (uint32_t)pid);

    /* vfork: the parent waits until the child has its own address space */
    while ((flags & CLONE_VFORK) && c->pid == pid && c->vfork_parent == p->pid && c->state != TASK_UNUSED)
        sleep_on(&c->vfork_parent);
    irq_restore(fl);
    return pid;

nomem:
    kfree(cf);
    files_put(files);
    fs_put(fs);
    sighand_put(sh);
    mm_put(mm);
    return -ENOMEM;
again:
    files_put(files);
    fs_put(fs);
    sighand_put(sh);
    return -EAGAIN;
}

/* ---- execve ---------------------------------------------------------------- */
static void free_strings(char **v, int n)
{
    if (!v) return;
    for (int i = 0; i < n; i++) kfree(v[i]);
    kfree(v);
}

/* Copy a NULL-terminated user vector of strings. */
static int64_t copy_vector(uint64_t uv, char ***out, int *count, size_t *bytes)
{
    *out = NULL;
    *count = 0;
    if (!uv) return 0;                                  /* Linux accepts NULL argv/envp */
    int n = 0;
    for (;; n++) {
        uint64_t p;
        if (n >= EXEC_MAX_STRINGS) return -E2BIG;
        if (get_user_u64(&p, (const void *)(uv + (uint64_t)n * 8))) return -EFAULT;
        if (!p) break;
    }
    char **v = kzalloc(sizeof(char *) * (size_t)(n + 1));
    if (!v) return -ENOMEM;
    for (int i = 0; i < n; i++) {
        int64_t err = 0;
        uint64_t p;
        if (get_user_u64(&p, (const void *)(uv + (uint64_t)i * 8)) || !p) { free_strings(v, i); return -EFAULT; }
        v[i] = ustrdup(p, EXEC_MAX_BYTES - *bytes, &err);
        if (!v[i]) { free_strings(v, i); return err; }
        *bytes += strlen(v[i]) + 1;
        if (*bytes > EXEC_MAX_BYTES) { free_strings(v, i + 1); return -E2BIG; }
    }
    *out = v;
    *count = n;
    return 0;
}

static const uint8_t *clean_fpu(void)
{
    static uint8_t img[512] __attribute__((aligned(16)));
    static bool done;
    if (!done) {
        memset(img, 0, sizeof img);
        *(uint16_t *)(img + 0) = 0x037f;                /* FCW: x87 default      */
        *(uint32_t *)(img + 24) = 0x1f80;               /* MXCSR: SSE default    */
        done = true;
    }
    return img;
}

/* execve in a multi-threaded process: end the other threads first. The
 * caller becomes the only thread, and takes over the process id if it was
 * not the leader. Fails if the whole process is being killed meanwhile. */
static int de_thread(struct tcb *t)
{
    struct signal_struct *g = t->signal;
    uint64_t fl = irq_save();
    if (g->nr_threads > 1) {
        if (g->group_exit || g->exec_kill) { irq_restore(fl); return -EAGAIN; }
        g->exec_kill = true;
        kill_other_threads(t);
        while (g->nr_threads > 1) {
            if (g->group_exit) { g->exec_kill = false; irq_restore(fl); return -EINTR; }
            sleep_on(&g->exit_wait);
        }
        g->exec_kill = false;
    }
    if (!is_leader(t)) {
        struct tcb *lead = task_find(t->tgid);          /* exited: a zombie kept for the pid */
        int old = t->pid;
        t->pid = t->tgid;
        if (lead) {
            t->ppid = lead->ppid;
            t->exit_signal = lead->exit_signal;
            t->start_tick = lead->start_tick;
            t->waiters += lead->waiters;                /* task_wait() callers follow the pid */
            lead->waiters = 0;
            lead->pid = old;                            /* now an ordinary dead thread: reaped */
            wakeup(lead);
        }
    }
    irq_restore(fl);
    return 0;
}

int64_t proc_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    struct tcb *t = current_task();
    struct int_frame *f = t->uframe;
    if (!t->user || !f) return -EINVAL;

    int64_t rc = 0;
    char *path = ustrdup(upath, VFS_PATH_MAX, &rc);
    if (!path) return rc == -E2BIG ? -ENAMETOOLONG : rc;
    struct exec_args ea = { 0, 0, NULL, NULL };
    size_t bytes = 0;
    if ((rc = copy_vector(uargv, &ea.argv, &ea.argc, &bytes)) < 0 ||
        (rc = copy_vector(uenvp, &ea.envp, &ea.envc, &bytes)) < 0)
        goto out;

    struct stat st;
    if ((rc = vfs_stat(path, &st)) < 0) goto out;
    if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) { rc = -EACCES; goto out; }

    /* Build the new image in a fresh address space; the old one stays
     * intact (and current again) if loading fails. */
    struct mm *old = t->mm, *nw = mm_create();
    if (!nw) { rc = -ENOMEM; goto out; }
    uint64_t fl = irq_save();
    t->mm = nw;
    t->pml4 = t->cr3 = nw->pml4;
    write_cr3(nw->pml4);
    irq_restore(fl);
    uint64_t entry = 0, sp = 0;
    rc = exec_load_image(t, path, &ea, &entry, &sp);
    if (rc >= 0) rc = de_thread(t);                     /* other threads go before the old image */
    if (rc < 0) {
        fl = irq_save();
        t->mm = old;
        t->pml4 = t->cr3 = old->pml4;
        write_cr3(old->pml4);
        irq_restore(fl);
        mm_put(nw);
        goto out;
    }

    /* point of no return: drop the old image (a vfork parent keeps using it) */
    mm_put(old);
    vfork_release(t);
    /* Tables shared with another process (CLONE_FILES, CLONE_SIGHAND without
     * threads) are copied first: the new program must not change theirs. */
    if (t->files->refs > 1) {
        struct files_struct *nf = files_dup(t->files);
        if (nf) { struct files_struct *of = t->files; t->files = nf; files_put(of); }
    }
    if (t->sighand->refs > 1) {
        struct sighand_struct *nh = sighand_new(t->sighand);
        if (nh) { struct sighand_struct *oh = t->sighand; t->sighand = nh; sighand_put(oh); }
    }
    struct files_struct *ft = t->files;
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (ft->fd[fd] && ft->cloexec[fd]) {
            struct file *x = ft->fd[fd];
            ft->fd[fd] = NULL;
            ft->cloexec[fd] = 0;
            vfs_close(x);
        }
    struct k_sigaction *act = t->sighand->action;
    for (int s = 0; s < NSIG; s++) {                    /* caught signals revert to default */
        if (act[s].handler != SIG_IGN) act[s].handler = SIG_DFL;
        act[s].flags = 0;
        act[s].restorer = 0;
        act[s].mask = 0;
    }
    t->saved_mask_valid = false;
    t->robust_list = 0;
    strlcpy(t->exe, path, sizeof t->exe);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    strlcpy(t->name, base, sizeof t->name);
    fl = irq_save();
    memcpy(t->fpu, clean_fpu(), sizeof t->fpu);
    __asm__ volatile("fxrstor %0" :: "m"(t->fpu));
    t->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    t->tid_address = 0;
    irq_restore(fl);

    uint64_t cs = f->cs, ss = f->ss;
    memset(f, 0, sizeof *f);
    f->cs = cs;
    f->ss = ss;
    f->rip = entry;
    f->rsp = sp;
    f->rflags = 0x202;
    kprintf("exec: pid %d: execve %s, entry %lx\n", t->pid, path, entry);
    rc = 0;
out:
    free_strings(ea.argv, ea.argc);
    free_strings(ea.envp, ea.envc);
    kfree(path);
    return rc;
}

/* ---- wait4 ----------------------------------------------------------------- */
static bool wait_matches(const struct tcb *self, const struct tcb *c, int64_t pid)
{
    if (c->state == TASK_UNUSED || !c->user || !is_leader(c) || c->ppid != self->tgid || same_group(c, self))
        return false;
    if (pid > 0) return c->pid == pid;
    if (pid == -1) return true;
    if (pid == 0) return c->pgid == self->pgid;
    return c->pgid == -pid;
}

int64_t proc_wait4(int64_t pid, uint64_t ustatus, uint64_t options, uint64_t rusage)
{
    struct tcb *self = current_task();
    if (!self->user) return -ECHILD;
    if (ustatus && !user_range_ok(ustatus, 4)) return -EFAULT;
    if (rusage && !user_range_ok(rusage, 144)) return -EFAULT;
    uint64_t fl = irq_save();
    for (;;) {
        bool any = false;
        struct tcb *z = NULL;
        for (int i = 0; i < MAX_TASKS && !z; i++) {
            struct tcb *c = task_slot(i);
            if (!wait_matches(self, c, pid)) continue;
            any = true;
            if (c->state == TASK_ZOMBIE && c->signal->nr_threads == 0) z = c;   /* every thread gone */
        }
        if (!any) { irq_restore(fl); return -ECHILD; }
        if (z) {
            int status = z->term_signal ? (z->term_signal & 0x7f) : ((z->exit_code & 0xff) << 8);
            int zpid = z->pid;
            task_reap(z);
            irq_restore(fl);
            if (ustatus && put_user_u32((void *)ustatus, (uint32_t)status)) return -EFAULT;
            if (rusage && clear_user((void *)rusage, 144)) return -EFAULT;
            return zpid;
        }
        if (options & WNOHANG) { irq_restore(fl); return 0; }
        if (signal_pending()) { irq_restore(fl); return -ERESTARTSYS; }
        sleep_on(self->signal);                         /* a child's exit wakes the process */
    }
}

/* ---- sessions and process groups ------------------------------------------ */
/* Set the process group and session of every thread of t's process. */
static void set_ids(struct tcb *t, int pgid, int sid, bool drop_ctty)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *o = task_slot(i);
        if (o->state == TASK_UNUSED || !same_group(o, t)) continue;
        o->pgid = pgid;
        if (sid) o->sid = sid;
        if (drop_ctty) o->ctty = 0;
    }
}

int64_t proc_setsid(void)
{
    struct tcb *t = current_task();
    uint64_t fl = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {               /* must not lead a group already */
        struct tcb *o = task_slot(i);
        if (o->state != TASK_UNUSED && o->user && o->pgid == t->tgid) { irq_restore(fl); return -EPERM; }
    }
    set_ids(t, t->tgid, t->tgid, true);                 /* a new session has no terminal */
    irq_restore(fl);
    return t->sid;
}

int64_t proc_setpgid(int64_t pid, int64_t pgid)
{
    struct tcb *self = current_task();
    struct tcb *t = pid ? process_of((int)pid) : self;
    if (!proc_alive(t) || !t->user || (!same_group(t, self) && t->ppid != self->tgid)) return -ESRCH;
    if (pgid < 0) return -EINVAL;
    if (t->sid == t->tgid) return -EPERM;               /* session leader */
    uint64_t fl = irq_save();
    set_ids(t, pgid ? (int)pgid : t->tgid, 0, false);
    irq_restore(fl);
    return 0;
}

int64_t proc_getpgid(int64_t pid)
{
    struct tcb *t = pid ? task_find((int)pid) : current_task();
    return t && t->state != TASK_UNUSED ? t->pgid : -ESRCH;
}

int64_t proc_getsid(int64_t pid)
{
    struct tcb *t = pid ? task_find((int)pid) : current_task();
    return t && t->state != TASK_UNUSED ? t->sid : -ESRCH;
}

/* ---- signals -----------------------------------------------------------------
 * The mask and a pending set are per thread; the actions (sighand) and a
 * second pending set (signal->shared_pending) belong to the process. A
 * signal sent to a process (kill, the terminal, SIGCHLD, SIGALRM) goes into
 * the shared set, and one thread that does not block it is woken to take it;
 * a signal sent to a thread (tgkill, a CPU fault) only that thread takes. A
 * fatal signal ends the whole process. */
static bool default_ignored(int sig)
{
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:  /* no job control */
        return true;
    default:
        return false;
    }
}

static bool ignored(const struct tcb *t, int sig)
{
    if (sig == SIGKILL) return false;
    const struct k_sigaction *k = &t->sighand->action[sig - 1];
    return k->handler == SIG_IGN || (k->handler == SIG_DFL && default_ignored(sig));
}

static uint64_t blocked_of(const struct tcb *t) { return t->sig_mask & ~UNBLOCKABLE; }

int signal_wait_chan;

int signal_send_thread(struct tcb *t, int sig, const struct ksig_info *info)
{
    if (!thread_alive(t) || !t->user) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > NSIG) return -EINVAL;
    if (ignored(t, sig)) return 0;                      /* discarded at generation */
    uint64_t fl = irq_save();
    if (!(t->sig_pending & sigbit(sig)))                /* standard signals do not queue */
        t->siginfo[sig - 1] = info ? *info : (struct ksig_info){ .code = SI_KERNEL };
    t->sig_pending |= sigbit(sig);
    if (!(blocked_of(t) & sigbit(sig))) task_interrupt(t);
    wakeup(&signal_wait_chan);                          /* signalfd readers, pollers */
    irq_restore(fl);
    return 0;
}

int signal_send_info(struct tcb *t, int sig, const struct ksig_info *info)
{
    if (!proc_alive(t) || !t->user) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > NSIG) return -EINVAL;
    if (ignored(t, sig)) return 0;
    struct signal_struct *g = t->signal;
    uint64_t fl = irq_save();
    if (sig == SIGKILL) {                               /* every thread, at once */
        for (int i = 0; i < MAX_TASKS; i++) {
            struct tcb *o = task_slot(i);
            if (thread_alive(o) && same_group(o, t)) signal_send_thread(o, SIGKILL, info);
        }
        irq_restore(fl);
        return 0;
    }
    if (!(g->shared_pending & sigbit(sig)))
        g->shared_info[sig - 1] = info ? *info : (struct ksig_info){ .code = SI_KERNEL };
    g->shared_pending |= sigbit(sig);
    /* wake one thread that will take it: t itself if it can */
    struct tcb *w = thread_alive(t) && !(blocked_of(t) & sigbit(sig)) ? t : NULL;
    for (int i = 0; i < MAX_TASKS && !w; i++) {
        struct tcb *o = task_slot(i);
        if (thread_alive(o) && same_group(o, t) && !(blocked_of(o) & sigbit(sig))) w = o;
    }
    if (w) task_interrupt(w);
    wakeup(&signal_wait_chan);
    irq_restore(fl);
    return 0;
}

int signal_send(struct tcb *t, int sig) { return signal_send_info(t, sig, NULL); }

/* A synchronous fault of the current thread (idt.c). Like Linux's
 * force_sig_fault(): a fault cannot be ignored or blocked -- if it is, the
 * action reverts to the default (the process dies) instead of returning to
 * the faulting instruction forever. */
void signal_force_fault(int sig, int code, uint64_t addr, uint32_t trapno, uint64_t err)
{
    struct tcb *t = current_task();
    struct k_sigaction *k = &t->sighand->action[sig - 1];
    uint64_t fl = irq_save();
    if (k->handler == SIG_IGN || (t->sig_mask & sigbit(sig))) {
        k->handler = SIG_DFL;
        t->sig_mask &= ~sigbit(sig);
    }
    t->siginfo[sig - 1] = (struct ksig_info){ .code = code, .addr = addr, .trapno = trapno, .err = err };
    t->sig_pending |= sigbit(sig);
    irq_restore(fl);
}

static uint64_t pending_of(const struct tcb *t) { return t->sig_pending | t->signal->shared_pending; }

bool signal_pending(void)
{
    struct tcb *t = current_task();
    return t->user && (pending_of(t) & ~blocked_of(t));
}

uint64_t signal_pending_mask(void)
{
    struct tcb *t = current_task();
    return t->user ? pending_of(t) : 0;
}

int signal_dequeue(uint64_t mask, struct ksig_info *info)
{
    struct tcb *t = current_task();
    if (!t->user) return 0;
    uint64_t fl = irq_save();
    uint64_t ready = pending_of(t) & mask & ~UNBLOCKABLE;
    int sig = 0;
    if (ready) {
        sig = __builtin_ctzll(ready) + 1;
        if (t->sig_pending & sigbit(sig)) { t->sig_pending &= ~sigbit(sig); *info = t->siginfo[sig - 1]; }
        else { t->signal->shared_pending &= ~sigbit(sig); *info = t->signal->shared_info[sig - 1]; }
    }
    irq_restore(fl);
    return sig;
}

static bool fault_signal(int sig) { return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP; }

/* The thread dies of sig; unless execve is clearing out the other threads,
 * the whole process goes with it (status: killed by sig). */
__attribute__((noreturn)) static void die(struct tcb *t, int sig, uint64_t rip, const struct ksig_info *i)
{
    struct signal_struct *g = t->signal;
    cli();
    if (!g->group_exit && !(g->exec_kill && sig == SIGKILL)) {
        if (fault_signal(sig) && i->trapno)
            kprintf("process %d (%s): thread %d killed by signal %d (code %d, address %lx) at RIP=%lx\n",
                    t->tgid, t->name, t->pid, sig, i->code, i->addr, rip);
        else
            kprintf("process %d (%s): killed by signal %d\n", t->tgid, t->name, sig);
        g->group_exit = true;
        g->group_exit_code = 128 + sig;
        g->group_term_signal = sig;
        kill_other_threads(t);
    }
    t->term_signal = sig;
    sti();
    task_exit(128 + sig);
}

/* Linux x86_64 signal frame (arch/x86/include/uapi/asm/sigcontext.h). */
struct sigctx {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct kucontext {
    uint64_t uc_flags, uc_link;
    uint64_t ss_sp;
    int32_t  ss_flags, ss_pad;
    uint64_t ss_size;
    struct sigctx mc;
    uint64_t sigmask;
};

struct ksiginfo {
    int32_t si_signo, si_errno, si_code, si_pad;
    union {
        struct { int32_t pid; uint32_t uid; int32_t status, pad; int64_t utime, stime; } kill;   /* + SIGCHLD */
        struct { uint64_t addr; } fault;
        uint8_t raw[128 - 16];
    } u;
};

struct rt_sigframe {
    uint64_t pretcode;                  /* the handler returns here: SA_RESTORER */
    struct kucontext uc;
    struct ksiginfo info;
};

_Static_assert(sizeof(struct sigctx) == 256, "sigcontext layout");
_Static_assert(sizeof(struct kucontext) == 304, "ucontext layout");
_Static_assert(sizeof(struct ksiginfo) == 128, "siginfo layout");

static bool user_code(uint64_t a) { return uvm_range_ok(a, 1); }

/* Build the frame in kernel memory, then copy it to the user stack in one
 * checked copy: a bad or unmapped stack (or one another thread unmaps right
 * now) makes delivery fail cleanly, and the process dies of SIGSEGV. */
static bool setup_frame(struct tcb *t, struct int_frame *f, int sig, struct k_sigaction *k,
                        const struct ksig_info *si)
{
    if (!(k->flags & SA_RESTORER) || !user_code(k->handler) || !user_code(k->restorer)) return false;
    uint64_t sp = f->rsp - 128;                         /* skip the red zone */
    uint64_t fpu = (sp - 512) & ~63ull;
    uint64_t frame = ((fpu - sizeof(struct rt_sigframe)) & ~15ull) - 8;   /* rsp+8 16-aligned */
    if (frame < UVM_USER_START || f->rsp > UVM_USER_END || !user_range_ok(frame, f->rsp - frame)) return false;

    static uint8_t fxbuf[512] __attribute__((aligned(16)));
    struct rt_sigframe fr;
    memset(&fr, 0, sizeof fr);
    uint64_t fl = irq_save();                           /* fxbuf is shared */
    __asm__ volatile("fxsave %0" : "=m"(fxbuf));
    int bad = copy_to_user((void *)fpu, fxbuf, sizeof fxbuf);
    irq_restore(fl);
    if (bad) return false;

    fr.pretcode = k->restorer;
    struct sigctx *m = &fr.uc.mc;
    m->r8 = f->r8; m->r9 = f->r9; m->r10 = f->r10; m->r11 = f->r11;
    m->r12 = f->r12; m->r13 = f->r13; m->r14 = f->r14; m->r15 = f->r15;
    m->rdi = f->rdi; m->rsi = f->rsi; m->rbp = f->rbp; m->rbx = f->rbx;
    m->rdx = f->rdx; m->rax = f->rax; m->rcx = f->rcx; m->rsp = f->rsp;
    m->rip = f->rip; m->eflags = f->rflags;
    m->cs = (uint16_t)f->cs; m->ss = (uint16_t)f->ss;
    m->err = si->trapno ? si->err : 0;
    m->trapno = si->trapno;
    m->cr2 = si->trapno == 14 ? si->addr : 0;
    uint64_t oldmask = t->saved_mask_valid ? t->saved_mask : t->sig_mask;
    m->oldmask = oldmask;
    m->fpstate = fpu;
    fr.uc.sigmask = oldmask;
    fr.info.si_signo = sig;
    fr.info.si_code = si->code;
    if (fault_signal(sig) && si->trapno) {
        fr.info.u.fault.addr = si->addr;
    } else {
        fr.info.u.kill.pid = si->pid;
        fr.info.u.kill.uid = si->uid;
        fr.info.u.kill.status = si->status;
    }
    if (copy_to_user((void *)frame, &fr, sizeof fr)) return false;

    f->rip = k->handler;
    f->rsp = frame;
    f->rdi = (uint64_t)sig;
    f->rsi = frame + __builtin_offsetof(struct rt_sigframe, info);
    f->rdx = frame + __builtin_offsetof(struct rt_sigframe, uc);
    f->rax = 0;
    f->rflags &= ~(uint64_t)(0x400 | 0x100);            /* DF, TF */

    t->saved_mask_valid = false;
    t->sig_mask |= k->mask | ((k->flags & SA_NODEFER) ? 0 : sigbit(sig));
    t->sig_mask &= ~UNBLOCKABLE;
    if (k->flags & SA_RESETHAND) { k->handler = SIG_DFL; k->flags &= ~(uint64_t)SA_SIGINFO; }
    __asm__ volatile("fxrstor %0" :: "m"(*(const uint8_t (*)[512])clean_fpu()));
    return true;
}

static void restart(struct int_frame *f, uint64_t nr)
{
    f->rip -= 2;                                        /* `syscall` and `int 0x80` */
    f->rax = nr;
}

void signal_deliver(struct int_frame *f, uint64_t nr, bool syscall)
{
    struct tcb *t = current_task();
    if (!t->user || (f->cs & 3) != 3) return;
    int64_t ret = syscall ? (int64_t)f->rax : 0;
    struct signal_struct *g = t->signal;
    for (;;) {
        uint64_t ready = pending_of(t) & ~blocked_of(t);
        if (!ready) break;
        int sig = (ready & sigbit(SIGKILL)) ? SIGKILL : __builtin_ctzll(ready) + 1;
        struct ksig_info info;
        if (t->sig_pending & sigbit(sig)) {             /* this thread's own first */
            t->sig_pending &= ~sigbit(sig);
            info = t->siginfo[sig - 1];
        } else {
            g->shared_pending &= ~sigbit(sig);
            info = g->shared_info[sig - 1];
        }
        struct k_sigaction *k = &t->sighand->action[sig - 1];
        if (sig == SIGKILL || k->handler == SIG_DFL) {
            if (sig != SIGKILL && default_ignored(sig)) continue;
            die(t, sig, f->rip, &info);
        }
        if (k->handler == SIG_IGN) continue;
        if (syscall) {
            if (ret == -ERESTARTSYS && (k->flags & SA_RESTART)) restart(f, nr);
            else if (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND) f->rax = (uint64_t)-EINTR;
        }
        if (!setup_frame(t, f, sig, k, &info)) {
            struct ksig_info segv = { .code = SI_KERNEL };
            die(t, SIGSEGV, f->rip, &segv);
        }
        return;                                         /* one handler per return */
    }
    if (syscall && (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND)) restart(f, nr);
    if (t->saved_mask_valid) {                          /* no handler ran: the wait's mask goes */
        t->sig_mask = t->saved_mask;
        t->saved_mask_valid = false;
    }
}

int64_t sig_return(void)
{
    struct tcb *t = current_task();
    struct int_frame *f = t->uframe;
    uint64_t ucp = f->rsp;                              /* the handler's `ret` popped pretcode */
    struct kucontext uc;
    if (copy_from_user(&uc, (const void *)ucp, sizeof uc)) goto bad;
    const struct sigctx *m = &uc.mc;
    if (!user_code(m->rip) || !uvm_range_ok(m->rsp, 1)) goto bad;

    if (m->fpstate) {
        static uint8_t buf[512] __attribute__((aligned(16)));
        if (m->fpstate & 15) goto bad;
        uint64_t fl = irq_save();
        if (copy_from_user(buf, (const void *)m->fpstate, sizeof buf)) { irq_restore(fl); goto bad; }
        *(uint32_t *)(buf + 24) &= 0xffbf;              /* reserved MXCSR bits would #GP */
        __asm__ volatile("fxrstor %0" :: "m"(buf));
        irq_restore(fl);
    }
    f->r8 = m->r8; f->r9 = m->r9; f->r10 = m->r10; f->r11 = m->r11;
    f->r12 = m->r12; f->r13 = m->r13; f->r14 = m->r14; f->r15 = m->r15;
    f->rdi = m->rdi; f->rsi = m->rsi; f->rbp = m->rbp; f->rbx = m->rbx;
    f->rdx = m->rdx; f->rcx = m->rcx; f->rsp = m->rsp; f->rip = m->rip;
    const uint64_t user_flags = 0x50dd5;                /* CF PF AF ZF SF TF DF OF AC RF */
    f->rflags = (f->rflags & ~user_flags) | (m->eflags & user_flags) | 0x200;
    t->sig_mask = uc.sigmask & ~UNBLOCKABLE;
    t->iret_return = true;                              /* rcx and r11 matter now */
    return (int64_t)m->rax;
bad:;
    struct ksig_info segv = { .code = SI_KERNEL };
    die(t, SIGSEGV, f->rip, &segv);
}

int64_t sig_action(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8 || sig < 1 || sig > NSIG) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    struct k_sigaction k, old = t->sighand->action[sig - 1];
    if (act && copy_from_user(&k, (const void *)act, sizeof k)) return -EFAULT;
    if (oact && copy_to_user((void *)oact, &old, sizeof old)) return -EFAULT;
    if (act) {
        k.mask &= ~UNBLOCKABLE;
        uint64_t fl = irq_save();
        t->sighand->action[sig - 1] = k;
        if (ignored(t, (int)sig)) {                     /* pending ones are discarded, in every thread */
            t->signal->shared_pending &= ~sigbit((int)sig);
            for (int i = 0; i < MAX_TASKS; i++) {
                struct tcb *o = task_slot(i);
                if (o->state != TASK_UNUSED && o->sighand == t->sighand) o->sig_pending &= ~sigbit((int)sig);
            }
        }
        irq_restore(fl);
    }
    return 0;
}

int64_t sig_procmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8) return -EINVAL;
    uint64_t s = 0, old = t->sig_mask;
    if (set && copy_from_user(&s, (const void *)set, 8)) return -EFAULT;
    if (set && how > 2) return -EINVAL;
    if (oset && copy_to_user((void *)oset, &old, 8)) return -EFAULT;
    if (set) {
        switch (how) {
        case 0: t->sig_mask |= s; break;                /* SIG_BLOCK   */
        case 1: t->sig_mask &= ~s; break;               /* SIG_UNBLOCK */
        case 2: t->sig_mask = s; break;                 /* SIG_SETMASK */
        }
        t->sig_mask &= ~UNBLOCKABLE;
    }
    return 0;
}

int64_t sig_pending_set(uint64_t set, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8) return -EINVAL;
    uint64_t v = pending_of(t) & t->sig_mask;
    return copy_to_user((void *)set, &v, 8) ? -EFAULT : 0;
}

/* Sleep until a signal is delivered to a handler (or ends the process). */
static int64_t wait_for_signal(void)
{
    uint64_t fl = irq_save();
    while (!signal_pending()) sleep_on(&current_task()->sig_pending);   /* signal_send interrupts */
    irq_restore(fl);
    return -ERESTARTNOHAND;
}

int64_t sig_suspend(uint64_t mask, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8) return -EINVAL;
    uint64_t m;
    if (copy_from_user(&m, (const void *)mask, 8)) return -EFAULT;
    t->saved_mask = t->sig_mask;
    t->saved_mask_valid = true;
    t->sig_mask = m & ~UNBLOCKABLE;
    int64_t r = wait_for_signal();
    /* restored by rt_sigreturn through the frame's mask if a handler runs */
    return r;
}

int64_t sig_pause(void) { return wait_for_signal(); }

int sig_temp_mask(uint64_t umask, uint64_t size)
{
    struct tcb *t = current_task();
    if (!umask) return 0;
    if (size != 8) return -EINVAL;
    uint64_t m;
    if (copy_from_user(&m, (const void *)umask, 8)) return -EFAULT;
    t->saved_mask = t->sig_mask;
    t->saved_mask_valid = true;
    t->sig_mask = m & ~UNBLOCKABLE;
    return 0;
}

void sig_temp_mask_end(void)
{
    struct tcb *t = current_task();
    if (!t->saved_mask_valid || signal_pending()) return;   /* delivery restores it */
    t->sig_mask = t->saved_mask;
    t->saved_mask_valid = false;
}

int64_t sig_kill(int64_t pid, int64_t sig)
{
    struct tcb *self = current_task();
    if (sig < 0 || sig > NSIG) return -EINVAL;
    struct ksig_info info = { .code = SI_USER, .pid = self->tgid, .uid = self->uid };
    if (pid > 0) {
        struct tcb *t = process_of((int)pid);
        if (!t || t->state == TASK_UNUSED) return -ESRCH;
        if (!t->user) return -EPERM;                    /* kernel threads */
        if (!proc_alive(t)) return 0;                   /* a zombie */
        return signal_send_info(t, (int)sig, &info);
    }
    int pgid = pid == 0 ? self->pgid : (int)-pid;
    bool found = false;
    for (int i = 0; i < MAX_TASKS; i++) {               /* each process once: through its leader */
        struct tcb *t = task_slot(i);
        if (!proc_alive(t) || !t->user || !is_leader(t)) continue;
        if (pid == -1 ? same_group(t, self) : t->pgid != pgid) continue;
        found = true;
        signal_send_info(t, (int)sig, &info);
    }
    return found ? 0 : -ESRCH;
}

/* tgkill(tgid, tid, sig) and tkill(tid, sig) (tgid 0): one thread. */
int64_t sig_tgkill(int64_t tgid, int64_t tid, int64_t sig)
{
    struct tcb *self = current_task();
    if (sig < 0 || sig > NSIG) return -EINVAL;
    if (tid <= 0 || tgid < 0) return -EINVAL;
    struct tcb *t = task_find((int)tid);
    if (!thread_alive(t) || (tgid > 0 && t->tgid != tgid)) return -ESRCH;
    if (!t->user) return -EPERM;
    struct ksig_info info = { .code = SI_TKILL, .pid = self->tgid, .uid = self->uid };
    return signal_send_thread(t, (int)sig, &info);
}
