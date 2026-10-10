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

extern void user_return(struct int_frame *f) __attribute__((noreturn));

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_SUPPORTED      (0xff | CLONE_VM | CLONE_VFORK | CLONE_SETTLS | CLONE_PARENT_SETTID | \
                              CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)

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

/* ---- process tree ---------------------------------------------------------- */
static bool alive(const struct tcb *t) { return t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE; }

bool process_zombie_kept(const struct tcb *t)
{
    if (!t->user) return false;
    struct tcb *p = task_find(t->ppid);
    if (!alive(p) || !p->user) return false;
    const struct k_sigaction *k = &p->sigact[SIGCHLD - 1];
    return !(k->handler == SIG_IGN || (k->flags & SA_NOCLDWAIT));
}

static void vfork_release(struct tcb *t)
{
    if (t->vfork_parent) {
        t->vfork_parent = 0;
        wakeup(&t->vfork_parent);
    }
}

void process_exit(struct tcb *t)
{
    vfork_release(t);
    for (int i = 0; i < MAX_TASKS; i++) {               /* orphans: reaped by the kernel */
        struct tcb *c = task_slot(i);
        if (c != t && c->state != TASK_UNUSED && c->ppid == t->pid && c->pid) c->ppid = 0;
    }
    if (!t->user) return;
    struct tcb *p = task_find(t->ppid);
    if (alive(p) && p->user) {
        if (t->exit_signal) {
            struct ksig_info ci = { .code = t->term_signal ? CLD_KILLED : CLD_EXITED, .pid = t->pid, .uid = t->uid,
                                    .status = t->term_signal ? t->term_signal : (t->exit_code & 0xff) };
            signal_send_info(p, t->exit_signal, &ci);
        }
        wakeup(p);                                      /* wait4() sleeps on itself */
    }
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
    if (flags & (CLONE_THREAD | CLONE_SIGHAND | CLONE_FILES | CLONE_FS)) return -ENOSYS;   /* threads */
    if (flags & ~(uint64_t)CLONE_SUPPORTED) return -EINVAL;
    if ((flags & CLONE_VM) && !(flags & CLONE_VFORK)) return -ENOSYS;                 /* threads */
    if ((flags & CLONE_PARENT_SETTID) && !user_range_ok(ptid, 4)) return -EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !user_range_ok(ctid, 4)) return -EFAULT;
    if ((flags & CLONE_SETTLS) && tls && !uvm_range_ok(tls, 1)) return -EINVAL;
    if (newsp && !user_range_ok(newsp, 0)) return -EINVAL;

    bool share = flags & CLONE_VM;
    struct mm *mm;
    if (share) { mm = p->mm; mm_get(mm); }
    else if (!(mm = mm_fork(p->mm))) return -ENOMEM;   /* copy-on-write */
    struct int_frame *cf = kmalloc(sizeof *cf);
    if (!cf) { mm_put(mm); return -ENOMEM; }
    *cf = *p->uframe;
    cf->rax = 0;                                        /* the child's return value */
    if (newsp) cf->rsp = newsp;
    cf->vector = (flags & CLONE_CHILD_SETTID) ? ctid : 0;   /* fork_child_entry writes the tid there */

    uint64_t fl = irq_save();                           /* the child must not run half-built */
    struct tcb *c = task_create(p->name, fork_child_entry, cf);
    if (!c) {
        irq_restore(fl);
        kfree(cf);
        mm_put(mm);
        return -EAGAIN;
    }
    c->ppid = p->pid;
    c->pgid = p->pgid;
    c->sid = p->sid;
    c->user = true;
    c->mm = mm;
    c->pml4 = c->cr3 = mm->pml4;
    c->fs_base = (flags & CLONE_SETTLS) ? tls : rdmsr(MSR_FS_BASE);
    c->tid_address = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    __asm__ volatile("fxsave %0" : "=m"(c->fpu));       /* the parent's live SSE state */
    for (int fd = 0; fd < MAX_FDS; fd++) {
        c->fds[fd] = p->fds[fd];
        c->fd_cloexec[fd] = p->fd_cloexec[fd];
        if (c->fds[fd]) c->fds[fd]->refcnt++;
    }
    c->cwd = p->cwd;
    c->uid = p->uid;
    c->gid = p->gid;
    c->umask = p->umask;
    c->tty = p->tty;
    c->ctty = p->ctty;
    memcpy(c->exe, p->exe, sizeof c->exe);
    memcpy(c->cmdline, p->cmdline, sizeof c->cmdline);
    c->cmdline_len = p->cmdline_len;
    c->alarm_at = c->alarm_every = 0;                   /* timers are not inherited */
    c->exit_signal = (int)(flags & 0xff);
    memcpy(c->sigact, p->sigact, sizeof c->sigact);
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
    for (int fd = 0; fd < MAX_FDS; fd++)
        if (t->fds[fd] && t->fd_cloexec[fd]) { vfs_close(t->fds[fd]); t->fds[fd] = NULL; t->fd_cloexec[fd] = 0; }
    for (int s = 0; s < NSIG; s++) {                    /* caught signals revert to default */
        if (t->sigact[s].handler != SIG_IGN) t->sigact[s].handler = SIG_DFL;
        t->sigact[s].flags = 0;
        t->sigact[s].restorer = 0;
        t->sigact[s].mask = 0;
    }
    t->saved_mask_valid = false;
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
    if (c->state == TASK_UNUSED || c->ppid != self->pid || c == self || !c->user) return false;
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
            if (c->state == TASK_ZOMBIE) z = c;
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
        sleep_on(self);                                 /* a child's exit wakes us */
    }
}

/* ---- sessions and process groups ------------------------------------------ */
int64_t proc_setsid(void)
{
    struct tcb *t = current_task();
    for (int i = 0; i < MAX_TASKS; i++) {               /* must not lead a group already */
        struct tcb *o = task_slot(i);
        if (o->state != TASK_UNUSED && o->pgid == t->pid && o->user) return -EPERM;
    }
    t->sid = t->pgid = t->pid;
    t->ctty = 0;                                        /* a new session has no terminal */
    return t->sid;
}

int64_t proc_setpgid(int64_t pid, int64_t pgid)
{
    struct tcb *self = current_task();
    struct tcb *t = pid ? task_find((int)pid) : self;
    if (!alive(t) || !t->user || (t != self && t->ppid != self->pid)) return -ESRCH;
    if (pgid < 0) return -EINVAL;
    if (t->sid == t->pid) return -EPERM;                /* session leader */
    t->pgid = pgid ? (int)pgid : t->pid;
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

/* ---- signals ------------------------------------------------------------- */
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
    const struct k_sigaction *k = &t->sigact[sig - 1];
    return k->handler == SIG_IGN || (k->handler == SIG_DFL && default_ignored(sig));
}

int signal_send_info(struct tcb *t, int sig, const struct ksig_info *info)
{
    if (!alive(t) || !t->user) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > NSIG) return -EINVAL;
    if (ignored(t, sig)) return 0;                      /* discarded at generation */
    uint64_t fl = irq_save();
    if (!(t->sig_pending & sigbit(sig))) {              /* standard signals do not queue */
        if (info) t->siginfo[sig - 1] = *info;
        else t->siginfo[sig - 1] = (struct ksig_info){ .code = SI_KERNEL };
    }
    t->sig_pending |= sigbit(sig);
    if (sig == SIGKILL || !(t->sig_mask & sigbit(sig))) task_interrupt(t);
    irq_restore(fl);
    return 0;
}

int signal_send(struct tcb *t, int sig) { return signal_send_info(t, sig, NULL); }

/* A synchronous fault of the current process (idt.c). Like Linux's
 * force_sig_fault(): a fault cannot be ignored or blocked -- if it is, the
 * action reverts to the default (the process dies) instead of returning to
 * the faulting instruction forever. */
void signal_force_fault(int sig, int code, uint64_t addr, uint32_t trapno, uint64_t err)
{
    struct tcb *t = current_task();
    struct k_sigaction *k = &t->sigact[sig - 1];
    uint64_t fl = irq_save();
    if (k->handler == SIG_IGN || (t->sig_mask & sigbit(sig))) {
        k->handler = SIG_DFL;
        t->sig_mask &= ~sigbit(sig);
    }
    t->siginfo[sig - 1] = (struct ksig_info){ .code = code, .addr = addr, .trapno = trapno, .err = err };
    t->sig_pending |= sigbit(sig);
    irq_restore(fl);
}

bool signal_pending(void)
{
    struct tcb *t = current_task();
    return t->user && (t->sig_pending & ~(t->sig_mask & ~UNBLOCKABLE));
}

static bool fault_signal(int sig) { return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP; }

__attribute__((noreturn)) static void die(struct tcb *t, int sig, uint64_t rip)
{
    const struct ksig_info *i = &t->siginfo[sig - 1];
    if (fault_signal(sig) && i->trapno)
        kprintf("process %d (%s): killed by signal %d (code %d, address %lx) at RIP=%lx\n",
                t->pid, t->name, sig, i->code, i->addr, rip);
    else
        kprintf("process %d (%s): killed by signal %d\n", t->pid, t->name, sig);
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
static bool setup_frame(struct tcb *t, struct int_frame *f, int sig, struct k_sigaction *k)
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

    const struct ksig_info *si = &t->siginfo[sig - 1];
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
    for (;;) {
        uint64_t ready = t->sig_pending & ~(t->sig_mask & ~UNBLOCKABLE);
        if (!ready) break;
        int sig = __builtin_ctzll(ready) + 1;
        t->sig_pending &= ~sigbit(sig);
        struct k_sigaction *k = &t->sigact[sig - 1];
        if (sig == SIGKILL || k->handler == SIG_DFL) {
            if (sig != SIGKILL && default_ignored(sig)) continue;
            die(t, sig, f->rip);
        }
        if (k->handler == SIG_IGN) continue;
        if (syscall) {
            if (ret == -ERESTARTSYS && (k->flags & SA_RESTART)) restart(f, nr);
            else if (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND) f->rax = (uint64_t)-EINTR;
        }
        if (!setup_frame(t, f, sig, k)) {
            t->siginfo[SIGSEGV - 1] = (struct ksig_info){ .code = SI_KERNEL };
            die(t, SIGSEGV, f->rip);
        }
        return;                                         /* one handler per return */
    }
    if (syscall && (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND)) restart(f, nr);
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
bad:
    t->siginfo[SIGSEGV - 1] = (struct ksig_info){ .code = SI_KERNEL };
    die(t, SIGSEGV, f->rip);
}

int64_t sig_action(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8 || sig < 1 || sig > NSIG) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    struct k_sigaction k, old = t->sigact[sig - 1];
    if (act && copy_from_user(&k, (const void *)act, sizeof k)) return -EFAULT;
    if (oact && copy_to_user((void *)oact, &old, sizeof old)) return -EFAULT;
    if (act) {
        k.mask &= ~UNBLOCKABLE;
        uint64_t fl = irq_save();
        t->sigact[sig - 1] = k;
        if (ignored(t, (int)sig)) t->sig_pending &= ~sigbit((int)sig);
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
    uint64_t v = t->sig_pending & t->sig_mask;
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

int64_t sig_kill(int64_t pid, int64_t sig)
{
    struct tcb *self = current_task();
    if (sig < 0 || sig > NSIG) return -EINVAL;
    struct ksig_info info = { .code = SI_USER, .pid = self->pid, .uid = self->uid };
    if (pid > 0) {
        struct tcb *t = task_find((int)pid);
        if (!t || t->state == TASK_UNUSED) return -ESRCH;
        if (!t->user) return -EPERM;                    /* kernel threads */
        if (t->state == TASK_ZOMBIE) return 0;
        return signal_send_info(t, (int)sig, &info);
    }
    int pgid = pid == 0 ? self->pgid : (int)-pid;
    bool found = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (!alive(t) || !t->user) continue;
        if (pid == -1 ? t == self : t->pgid != pgid) continue;
        found = true;
        signal_send_info(t, (int)sig, &info);
    }
    return found ? 0 : -ESRCH;
}

int64_t sig_tgkill(int64_t tgid, int64_t tid, int64_t sig)
{
    if (tid <= 0 || (tgid > 0 && tgid != tid)) return -ESRCH;   /* one thread per process */
    return sig_kill(tid, sig);
}
