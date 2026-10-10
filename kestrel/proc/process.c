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

/* ---- user memory --------------------------------------------------------- */
static bool uok(uint64_t p, uint64_t len)
{
    struct tcb *t = current_task();
    return t->user && p >= 4096 && len && uvm_mapped(t->pml4, p, len);
}

/* Length of the NUL-terminated user string at p (at most max-1 chars). */
static int64_t ustrlen(uint64_t p, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((p + i) & (UVM_PAGE - 1)) == 0) && !uok(p + i, 1)) return -EFAULT;
        if (((const char *)p)[i] == 0) return (int64_t)i;
    }
    return -E2BIG;
}

static char *ustrdup(uint64_t p, size_t max, int64_t *err)
{
    int64_t n = ustrlen(p, max);
    if (n < 0) { *err = n; return NULL; }
    char *s = kmalloc((size_t)n + 1);
    if (!s) { *err = -ENOMEM; return NULL; }
    memcpy(s, (const void *)p, (size_t)n + 1);
    return s;
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
        if (t->exit_signal) signal_send(p, t->exit_signal);
        wakeup(p);                                      /* wait4() sleeps on itself */
    }
}

/* ---- fork / vfork / clone ------------------------------------------------- */
static int fork_child_entry(void *arg)
{
    struct int_frame f = *(struct int_frame *)arg;
    kfree(arg);
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
    if ((flags & CLONE_CHILD_SETTID) && !(flags & CLONE_VM)) return -EINVAL;
    if ((flags & CLONE_PARENT_SETTID) && !uok(ptid, 4)) return -EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && !uok(ctid, 4)) return -EFAULT;
    if ((flags & CLONE_SETTLS) && tls && !uvm_range_ok(tls, 1)) return -EINVAL;

    bool share = flags & CLONE_VM;
    uint64_t pml4 = share ? p->pml4 : uvm_clone(p->pml4);      /* eager copy, interrupts on */
    if (!pml4) return -ENOMEM;
    struct int_frame *cf = kmalloc(sizeof *cf);
    if (!cf) { if (!share) uvm_destroy(pml4); return -ENOMEM; }
    *cf = *p->uframe;
    cf->rax = 0;                                        /* the child's return value */
    if (newsp) cf->rsp = newsp;

    uint64_t fl = irq_save();                           /* the child must not run half-built */
    struct tcb *c = task_create(p->name, fork_child_entry, cf);
    if (!c) {
        irq_restore(fl);
        kfree(cf);
        if (!share) uvm_destroy(pml4);
        return -EAGAIN;
    }
    c->ppid = p->pid;
    c->pgid = p->pgid;
    c->sid = p->sid;
    c->user = true;
    c->pml4 = c->cr3 = pml4;
    c->vm_borrowed = share;
    c->brk_start = p->brk_start;
    c->brk = p->brk;
    c->mmap_next = p->mmap_next;
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
    c->exit_signal = (int)(flags & 0xff);
    memcpy(c->sigact, p->sigact, sizeof c->sigact);
    c->sig_mask = p->sig_mask;
    c->sig_pending = 0;
    if (flags & CLONE_VFORK) c->vfork_parent = p->pid;
    int pid = c->pid;
    if (flags & CLONE_PARENT_SETTID) *(int32_t *)ptid = pid;
    if (flags & CLONE_CHILD_SETTID) *(int32_t *)ctid = pid;   /* shared memory (CLONE_VM) */

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
        if (n >= EXEC_MAX_STRINGS) return -E2BIG;
        if (!uok(uv + (uint64_t)n * 8, 8)) return -EFAULT;
        if (!((const uint64_t *)uv)[n]) break;
    }
    char **v = kzalloc(sizeof(char *) * (size_t)(n + 1));
    if (!v) return -ENOMEM;
    for (int i = 0; i < n; i++) {
        int64_t err = 0;
        v[i] = ustrdup(((const uint64_t *)uv)[i], EXEC_MAX_BYTES, &err);
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
    char *path = ustrdup(upath, 256, &rc);
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
    uint64_t old = t->pml4, nw = uvm_create();
    if (!nw) { rc = -ENOMEM; goto out; }
    uint64_t brk_start = t->brk_start, brk = t->brk, mmap_next = t->mmap_next;
    uint64_t fl = irq_save();
    t->pml4 = t->cr3 = nw;
    write_cr3(nw);
    irq_restore(fl);
    uint64_t entry = 0, sp = 0;
    rc = exec_load_image(t, path, &ea, &entry, &sp);
    if (rc < 0) {
        fl = irq_save();
        t->pml4 = t->cr3 = old;
        write_cr3(old);
        irq_restore(fl);
        uvm_destroy(nw);
        t->brk_start = brk_start; t->brk = brk; t->mmap_next = mmap_next;
        goto out;
    }

    /* point of no return */
    if (!t->vm_borrowed) uvm_destroy(old);
    t->vm_borrowed = false;
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
    kprintf("exec: pid %d: execve %s, entry %lx, %lu user pages\n", t->pid, path, entry, uvm_pages(t->pml4));
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
    if (ustatus && !uok(ustatus, 4)) return -EFAULT;
    if (rusage && !uok(rusage, 144)) return -EFAULT;
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
            if (ustatus) *(int32_t *)ustatus = status;
            if (rusage) memset((void *)rusage, 0, 144);
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

int signal_send(struct tcb *t, int sig)
{
    if (!alive(t) || !t->user) return -ESRCH;
    if (sig == 0) return 0;
    if (sig < 1 || sig > NSIG) return -EINVAL;
    if (ignored(t, sig)) return 0;                      /* discarded at generation */
    uint64_t fl = irq_save();
    t->sig_pending |= sigbit(sig);
    if (sig == SIGKILL || !(t->sig_mask & sigbit(sig))) task_interrupt(t);
    irq_restore(fl);
    return 0;
}

bool signal_pending(void)
{
    struct tcb *t = current_task();
    return t->user && (t->sig_pending & ~(t->sig_mask & ~UNBLOCKABLE));
}

__attribute__((noreturn)) static void die(struct tcb *t, int sig)
{
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
    int32_t si_pid, si_uid;
    uint8_t rest[128 - 24];
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

static bool setup_frame(struct tcb *t, struct int_frame *f, int sig, struct k_sigaction *k)
{
    if (!(k->flags & SA_RESTORER) || !user_code(k->handler) || !user_code(k->restorer)) return false;
    uint64_t sp = f->rsp - 128;                         /* skip the red zone */
    uint64_t fpu = (sp - 512) & ~63ull;
    uint64_t frame = ((fpu - sizeof(struct rt_sigframe)) & ~15ull) - 8;   /* rsp+8 16-aligned */
    if (frame < UVM_USER_START || !uok(frame, f->rsp - frame)) return false;

    __asm__ volatile("fxsave (%0)" :: "r"(fpu) : "memory");
    struct rt_sigframe *fr = (struct rt_sigframe *)frame;
    memset(fr, 0, sizeof *fr);
    fr->pretcode = k->restorer;
    struct sigctx *m = &fr->uc.mc;
    m->r8 = f->r8; m->r9 = f->r9; m->r10 = f->r10; m->r11 = f->r11;
    m->r12 = f->r12; m->r13 = f->r13; m->r14 = f->r14; m->r15 = f->r15;
    m->rdi = f->rdi; m->rsi = f->rsi; m->rbp = f->rbp; m->rbx = f->rbx;
    m->rdx = f->rdx; m->rax = f->rax; m->rcx = f->rcx; m->rsp = f->rsp;
    m->rip = f->rip; m->eflags = f->rflags;
    m->cs = (uint16_t)f->cs; m->ss = (uint16_t)f->ss;
    m->err = f->error; m->trapno = f->vector;
    uint64_t oldmask = t->saved_mask_valid ? t->saved_mask : t->sig_mask;
    m->oldmask = oldmask;
    m->fpstate = fpu;
    fr->uc.sigmask = oldmask;
    fr->info.si_signo = sig;

    f->rip = k->handler;
    f->rsp = frame;
    f->rdi = (uint64_t)sig;
    f->rsi = (uint64_t)&fr->info;
    f->rdx = (uint64_t)&fr->uc;
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
            die(t, sig);
        }
        if (k->handler == SIG_IGN) continue;
        if (syscall) {
            if (ret == -ERESTARTSYS && (k->flags & SA_RESTART)) restart(f, nr);
            else if (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND) f->rax = (uint64_t)-EINTR;
        }
        if (!setup_frame(t, f, sig, k)) die(t, SIGSEGV);
        return;                                         /* one handler per return */
    }
    if (syscall && (ret == -ERESTARTSYS || ret == -ERESTARTNOHAND)) restart(f, nr);
}

int64_t sig_return(void)
{
    struct tcb *t = current_task();
    struct int_frame *f = t->uframe;
    uint64_t ucp = f->rsp;                              /* the handler's `ret` popped pretcode */
    if (!uok(ucp, sizeof(struct kucontext))) die(t, SIGSEGV);
    struct kucontext uc = *(const struct kucontext *)ucp;
    const struct sigctx *m = &uc.mc;
    if (!user_code(m->rip) || !uvm_range_ok(m->rsp, 1)) die(t, SIGSEGV);

    if (m->fpstate) {
        static uint8_t buf[512] __attribute__((aligned(16)));
        if ((m->fpstate & 15) || !uok(m->fpstate, 512)) die(t, SIGSEGV);
        uint64_t fl = irq_save();
        memcpy(buf, (const void *)m->fpstate, sizeof buf);
        *(uint32_t *)(buf + 24) &= 0xffbf;              /* reserved MXCSR bits fault */
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
}

int64_t sig_action(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8 || sig < 1 || sig > NSIG) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    if (act && !uok(act, sizeof(struct k_sigaction))) return -EFAULT;
    if (oact && !uok(oact, sizeof(struct k_sigaction))) return -EFAULT;
    struct k_sigaction old = t->sigact[sig - 1];
    if (act) {
        struct k_sigaction k = *(const struct k_sigaction *)act;
        k.mask &= ~UNBLOCKABLE;
        uint64_t fl = irq_save();
        t->sigact[sig - 1] = k;
        if (ignored(t, (int)sig)) t->sig_pending &= ~sigbit((int)sig);
        irq_restore(fl);
    }
    if (oact) *(struct k_sigaction *)oact = old;
    return 0;
}

int64_t sig_procmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8) return -EINVAL;
    if (set && !uok(set, 8)) return -EFAULT;
    if (oset && !uok(oset, 8)) return -EFAULT;
    uint64_t old = t->sig_mask;
    if (set) {
        uint64_t s = *(const uint64_t *)set;
        switch (how) {
        case 0: t->sig_mask |= s; break;                /* SIG_BLOCK   */
        case 1: t->sig_mask &= ~s; break;               /* SIG_UNBLOCK */
        case 2: t->sig_mask = s; break;                 /* SIG_SETMASK */
        default: return -EINVAL;
        }
        t->sig_mask &= ~UNBLOCKABLE;
    }
    if (oset) *(uint64_t *)oset = old;
    return 0;
}

int64_t sig_pending_set(uint64_t set, uint64_t size)
{
    struct tcb *t = current_task();
    if (size != 8) return -EINVAL;
    if (!uok(set, 8)) return -EFAULT;
    *(uint64_t *)set = t->sig_pending & t->sig_mask;
    return 0;
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
    if (!uok(mask, 8)) return -EFAULT;
    t->saved_mask = t->sig_mask;
    t->saved_mask_valid = true;
    t->sig_mask = *(const uint64_t *)mask & ~UNBLOCKABLE;
    int64_t r = wait_for_signal();
    /* restored by rt_sigreturn through the frame's mask if a handler runs */
    return r;
}

int64_t sig_pause(void) { return wait_for_signal(); }

int64_t sig_kill(int64_t pid, int64_t sig)
{
    struct tcb *self = current_task();
    if (sig < 0 || sig > NSIG) return -EINVAL;
    if (pid > 0) {
        struct tcb *t = task_find((int)pid);
        if (!t || t->state == TASK_UNUSED) return -ESRCH;
        if (!t->user) return -EPERM;                    /* kernel threads */
        if (t->state == TASK_ZOMBIE) return 0;
        return signal_send(t, (int)sig);
    }
    int pgid = pid == 0 ? self->pgid : (int)-pid;
    bool found = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (!alive(t) || !t->user) continue;
        if (pid == -1 ? t == self : t->pgid != pgid) continue;
        found = true;
        signal_send(t, (int)sig);
    }
    return found ? 0 : -ESRCH;
}

int64_t sig_tgkill(int64_t tgid, int64_t tid, int64_t sig)
{
    if (tid <= 0 || (tgid > 0 && tgid != tid)) return -ESRCH;   /* one thread per process */
    return sig_kill(tid, sig);
}
