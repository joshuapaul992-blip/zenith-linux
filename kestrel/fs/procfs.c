/* fs/procfs.c -- /proc: process and kernel state, generated on every read */
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <kernel/gpu.h>
#include <kernel/task.h>
#include <kernel/uvm.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/arch.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/bootinfo.h>
#include <kernel/syscall.h>
#include "pci.h"
#include <kernel/usbhost.h>
#include <kernel/block.h>
#include <kernel/bootvol.h>

#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))

static size_t gen_version(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("%s version %s (%s) gcc %s, built " __DATE__ " " __TIME__ "\n",
      KESTREL_NAME, KESTREL_VERSION, KESTREL_MACHINE, __VERSION__);
    return n;
}

static size_t gen_uptime(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    uint64_t ms = uptime_ms();
    P("%lu.%02lu\n", ms / 1000, (ms % 1000) / 10);
    return n;
}

static size_t gen_meminfo(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("MemTotal:      %8lu kB\n", pmm_total_bytes() >> 10);
    P("MemFree:       %8lu kB\n", pmm_free_bytes() >> 10);
    P("KernelImage:   %8lu kB\n", (uint64_t)(_kernel_end - _kernel_start) >> 10);
    P("HeapTotal:     %8zu kB\n", heap_size() >> 10);
    P("HeapUsed:      %8zu kB\n", heap_used() >> 10);
    return n;
}

static size_t gen_cpuinfo(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("processor\t: 0\nvendor_id\t: %s\nmodel name\t: %s\n", g_boot.cpu_vendor, g_boot.cpu_brand);
    P("timer\t\t: PIT 8254 @ %d Hz\n", PIT_HZ);
    return n;
}

static size_t gen_mounts(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        P("%s %s %s %s 0 0\n", m->fstype, m->path, m->fstype, (m->flags & MNT_RDONLY) ? "ro" : "rw");
    return n;
}

static size_t gen_filesystems(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("\tramfs\nnodev\tdevfs\nnodev\tprocfs\nnodev\tsysfs\n");
    return n;
}

static size_t gen_tasks(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    struct tcb *t = kmalloc(sizeof(struct tcb) * MAX_TASKS);
    if (!t) return 0;
    int cnt = task_snapshot(t, MAX_TASKS);
    P("  PID  PPID STATE      CPU(ms)  SWITCHES  NAME\n");
    for (int i = 0; i < cnt; i++)
        P("%5d %5d %-9s %8lu %9lu  %s\n", t[i].pid, t[i].ppid, task_state_name(t[i].state),
          t[i].cpu_ticks * 1000 / PIT_HZ, t[i].switches, t[i].name);
    kfree(t);
    return n;
}

static size_t gen_interrupts(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    static const char *names[16] = { "timer", "keyboard", "cascade", "", "COM1", "", "", "",
                                     "rtc", "", "", "", "mouse", "fpu", "ata0", "ata1" };
    for (int i = 0; i < 16; i++)
        if (irq_count(i)) P("%3d: %10lu  XT-PIC  %s\n", i, irq_count(i), names[i]);
    P("context switches: %lu\n", sched_context_switches());
    return n;
}

/* Linux layout, in USER_HZ (100) units: user nice system idle iowait irq
 * softirq steal guest guest_nice. One CPU. */
static size_t gen_stat(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    uint64_t u, s, i;
    sched_cpu_times(&u, &s, &i);
    u = u * 100 / PIT_HZ; s = s * 100 / PIT_HZ; i = i * 100 / PIT_HZ;
    P("cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n", u, s, i);
    P("cpu0 %lu 0 %lu %lu 0 0 0 0 0 0\n", u, s, i);
    P("ctxt %lu\nbtime 0\nprocs_running 1\nprocs_blocked 0\n", sched_context_switches());
    return n;
}

static size_t gen_cmdline(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("%s\n", g_boot.cmdline);
    return n;
}

static size_t gen_kmsg(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    size_t len = klog_size();
    size_t start = len > cap ? len - cap : 0;         /* most recent part */
    return klog_read(start, buf, cap);
}

static size_t gen_syscalls(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P(" NR  NAME              STATUS        CALLS\n");
    for (int i = 0; i < SYS_MAX; i++) {
        const char *nm = syscall_name(i);
        if (!nm) continue;
        P("%3d  %-16s  %-12s %6lu\n", i, nm, syscall_implemented(i) ? "implemented" : "ENOSYS stub",
          syscall_count(i));
    }
    return n;
}

static size_t gen_pci(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("BB:DD.F VEND:DEV  CLASS    IRQ  DESCRIPTION\n");
    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        P("%02x:%02x.%x %04x:%04x %02x.%02x.%02x ", d->bus, d->device, d->function,
          d->vendor_id, d->device_id, d->class_code, d->subclass, d->prog_if);
        if (d->interrupt_pin) P("%3u", d->interrupt_line); else P("  -");
        P("  %s", pci_class_name(d->class_code, d->subclass, d->prog_if));
        for (uint32_t bit = 1; bit < (1u << PCI_MATCH_CATEGORIES); bit <<= 1)
            if (d->matches & bit) P(" <%s>", pci_match_name((enum pci_match)bit));
        P("\n");
    }
    return n;
}

static size_t gen_usb(char *buf, size_t cap, void *ctx) { (void)ctx; return usb_proc(buf, cap); }
static size_t gen_partitions(char *buf, size_t cap, void *ctx) { (void)ctx; return blk_proc(buf, cap); }
static size_t gen_bootvol(char *buf, size_t cap, void *ctx) { (void)ctx; return bootvol_proc(buf, cap); }

/* NVIDIA: probe results and the modeset outcome (also the "gpu" command). */
static size_t gen_gpu(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    size_t n = gpu_modeset_report(buf, cap);           /* first: the full report is long */
    if (n && n < cap) buf[n++] = '\n';
    return n + gpu_report(buf + n, cap - n);
}

/* Load averages are not tracked (0.00); running/total and the last pid are. */
static size_t gen_loadavg(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    int total = 0, running = 0, last = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED) continue;
        total++;
        if (t->state == TASK_RUNNING || t->state == TASK_READY) running++;
        if (t->pid > last) last = t->pid;
    }
    P("0.00 0.00 0.00 %d/%d %d\n", running, total, last);
    return n;
}

/* ---- per-process directories: /proc/PID/{stat,status,cmdline,comm,maps} and
 * /proc/self -----------------------------------------------------------
 * A fixed pool of directories (one per task slot is enough) is attached to
 * /proc for live processes and detached again when they go; nothing is
 * freed, so a file kept open on a dead process just reads its successor's
 * data or nothing. The pool entry is the files' ctx; NULL means "self". */
struct pid_dir {
    struct vnode *dir;
    int  pid;
    bool attached;
};
static struct pid_dir pid_dirs[MAX_TASKS];
static struct vnode *proc_root;

static struct tcb *pid_task(void *ctx)
{
    if (!ctx) return current_task();
    struct pid_dir *d = ctx;
    struct tcb *t = task_find(d->pid);
    return t && t->state != TASK_UNUSED ? t : NULL;
}

static char state_char(const struct tcb *t)
{
    switch (t->state) {
    case TASK_RUNNING: case TASK_READY: return 'R';
    case TASK_ZOMBIE: return 'Z';
    default: return 'S';
    }
}

static size_t gen_pid_stat(char *buf, size_t cap, void *ctx)
{
    size_t n = 0;
    struct tcb *t = pid_task(ctx);
    if (!t) return 0;
    uint64_t ticks = t->cpu_ticks * 100 / PIT_HZ;              /* USER_HZ */
    uint64_t start = t->start_tick * 100 / PIT_HZ;
    uint64_t pages = t->mm ? t->mm->rss : 0;
    int tty = t->ctty ? (136 << 8) | (t->ctty - 1) : 0;
    /* pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice threads
     * itrealvalue starttime vsize rss ... (52 fields) */
    P("%d (%s) %c %d %d %d %d -1 %u 0 0 0 0 %lu 0 0 0 20 0 1 0 %lu %lu %lu",
      t->pid, t->name, state_char(t), t->ppid, t->pgid, t->sid, tty, t->user ? 0u : 0x200000u,
      ticks, start, pages * 4096, pages);
    P(" 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 %d\n", t->exit_code);
    return n;
}

static size_t gen_pid_status(char *buf, size_t cap, void *ctx)
{
    size_t n = 0;
    struct tcb *t = pid_task(ctx);
    if (!t) return 0;
    static const char *const names[] = { "?", "R (running)", "R (running)", "S (sleeping)", "S (sleeping)",
                                         "Z (zombie)" };
    uint64_t pages = t->mm ? t->mm->rss : 0;
    P("Name:\t%s\nState:\t%s\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n", t->name,
      t->state <= TASK_ZOMBIE ? names[t->state] : "?", t->pid, t->pid, t->ppid);
    P("Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n", t->uid, t->uid, t->uid, t->uid,
      t->gid, t->gid, t->gid, t->gid);
    if (t->user) P("VmSize:\t%lu kB\nVmRSS:\t%lu kB\n", pages * 4, pages * 4);
    P("Threads:\t1\nSigPnd:\t%016lx\nSigBlk:\t%016lx\n", t->sig_pending, t->sig_mask);
    return n;
}

static size_t gen_pid_cmdline(char *buf, size_t cap, void *ctx)
{
    struct tcb *t = pid_task(ctx);
    if (!t || !t->cmdline_len) return 0;                /* kernel threads: empty, as on Linux */
    size_t n = t->cmdline_len < cap ? t->cmdline_len : cap;
    memcpy(buf, t->cmdline, n);
    return n;
}

static size_t gen_pid_comm(char *buf, size_t cap, void *ctx)
{
    size_t n = 0;
    struct tcb *t = pid_task(ctx);
    if (t) P("%s\n", t->name);
    return n;
}

/* /proc/PID/maps: the regions of the address space, Linux format */
static size_t gen_pid_maps(char *buf, size_t cap, void *ctx)
{
    struct tcb *t = pid_task(ctx);
    return t && t->mm ? vm_maps(t->mm, buf, cap) : 0;
}

static void pid_files(struct vnode *dir, void *ctx)
{
    pseudo_file(dir, "stat", gen_pid_stat, ctx);
    pseudo_file(dir, "status", gen_pid_status, ctx);
    pseudo_file(dir, "cmdline", gen_pid_cmdline, ctx);
    pseudo_file(dir, "comm", gen_pid_comm, ctx);
    pseudo_file(dir, "maps", gen_pid_maps, ctx);
}

static void detach(struct vnode *parent, struct vnode *c)
{
    for (struct vnode **pp = &parent->children; *pp; pp = &(*pp)->sibling)
        if (*pp == c) { *pp = c->sibling; c->sibling = NULL; return; }
}

static void proc_refresh(struct vnode *dir)
{
    uint64_t fl = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {               /* gone: detach */
        struct pid_dir *d = &pid_dirs[i];
        if (!d->attached) continue;
        struct tcb *t = task_find(d->pid);
        if (!t || t->state == TASK_UNUSED) { detach(dir, d->dir); d->attached = false; }
    }
    for (int i = 0; i < MAX_TASKS; i++) {               /* new: attach a free entry */
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED) continue;
        bool have = false;
        for (int j = 0; j < MAX_TASKS && !have; j++) have = pid_dirs[j].attached && pid_dirs[j].pid == t->pid;
        if (have) continue;
        for (int j = 0; j < MAX_TASKS; j++) {
            struct pid_dir *d = &pid_dirs[j];
            if (d->attached) continue;
            if (!d->dir) {                              /* first use: build it */
                irq_restore(fl);
                d->dir = vfs_node_new(dir->fs, "", VDIR, 0555, NULL);
                if (d->dir) pid_files(d->dir, d);
                fl = irq_save();
                if (!d->dir) break;
            }
            d->pid = t->pid;
            snprintf(d->dir->name, sizeof d->dir->name, "%d", t->pid);
            d->dir->parent = dir;
            d->dir->sibling = dir->children;            /* attach */
            dir->children = d->dir;
            d->attached = true;
            break;
        }
    }
    irq_restore(fl);
}

static const struct vnode_ops proc_root_ops = { .refresh = proc_refresh };

void procfs_init(const char *mountpoint)
{
    struct mount *m;
    struct vnode *r = vfs_mkfs("procfs", MNT_RDONLY | MNT_NOCREATE, &m);
    pseudo_file(r, "version",     gen_version, NULL);
    pseudo_file(r, "uptime",      gen_uptime, NULL);
    pseudo_file(r, "meminfo",     gen_meminfo, NULL);
    pseudo_file(r, "stat",        gen_stat, NULL);
    pseudo_file(r, "loadavg",     gen_loadavg, NULL);
    pseudo_file(r, "gpu",         gen_gpu, NULL);
    pseudo_file(r, "cpuinfo",     gen_cpuinfo, NULL);
    pseudo_file(r, "mounts",      gen_mounts, NULL);
    pseudo_file(r, "filesystems", gen_filesystems, NULL);
    pseudo_file(r, "tasks",       gen_tasks, NULL);
    pseudo_file(r, "interrupts",  gen_interrupts, NULL);
    pseudo_file(r, "cmdline",     gen_cmdline, NULL);
    pseudo_file(r, "kmsg",        gen_kmsg, NULL);
    pseudo_file(r, "syscalls",    gen_syscalls, NULL);
    pseudo_file(r, "pci",         gen_pci, NULL);
    pseudo_file(r, "usb",         gen_usb, NULL);
    pseudo_file(r, "partitions",  gen_partitions, NULL);
    pseudo_file(r, "bootvol",     gen_bootvol, NULL);
    struct vnode *self = vfs_node_new(m, "self", VDIR, 0555, NULL);   /* the calling process */
    if (self) { vfs_node_add(r, self); pid_files(self, NULL); }
    proc_root = r;
    r->ops = &proc_root_ops;
    vfs_mount(m, mountpoint);
}
