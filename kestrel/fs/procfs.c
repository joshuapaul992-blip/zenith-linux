/* fs/procfs.c -- /proc: process and kernel state, generated on every read */
#include <kernel/smp.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <kernel/gpu.h>
#include <kernel/task.h>
#include <kernel/uvm.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/pagecache.h>
#include <kernel/msi.h>
#include <kernel/arch.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/bootinfo.h>
#include <kernel/syscall.h>
#include "pci.h"
#include <kernel/usbhost.h>
#include <kernel/block.h>
#include <kernel/bootvol.h>
#include <kernel/time.h>
#include <kernel/sound.h>

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
    uint64_t ms = uptime_ms(), u, sy, idle;
    sched_cpu_times(&u, &sy, &idle);
    idle = idle * 1000 / PIT_HZ;
    P("%lu.%02lu %lu.%02lu\n", ms / 1000, (ms % 1000) / 10, idle / 1000, (idle % 1000) / 10);
    return n;
}

static size_t gen_meminfo(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    struct pcache_stats pc;
    pcache_stats(&pc);
    uint64_t cached = pc.pages * PAGE_SIZE;
    P("MemTotal:      %8lu kB\n", pmm_total_bytes() >> 10);
    P("MemFree:       %8lu kB\n", pmm_free_bytes() >> 10);
    P("MemAvailable:  %8lu kB\n", (pmm_free_bytes() + cached) >> 10);   /* the page cache gives way */
    P("Buffers:       %8lu kB\n", 0ul);
    P("Cached:        %8lu kB\n", cached >> 10);
    P("KernelImage:   %8lu kB\n", (uint64_t)(_kernel_end - _kernel_start) >> 10);
    P("HeapTotal:     %8zu kB\n", heap_size() >> 10);
    P("HeapUsed:      %8zu kB\n", heap_used() >> 10);
    return n;
}

static size_t gen_cpuinfo(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    for (int i = 0; i < ncpus; i++) {                   /* one block per online CPU, as on Linux */
        P("processor\t: %d\nvendor_id\t: %s\nmodel name\t: %s\napicid\t\t: %u\n", i, g_boot.cpu_vendor,
          g_boot.cpu_brand, cpus[i].apic_id);
        P("timer\t\t: %s @ %d Hz\n\n", apic_enabled() ? "local APIC" : "PIT 8254", PIT_HZ);
    }
    return n;
}

static size_t gen_mounts(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        P("%s %s %s %s 0 0\n", m->fstype, m->path, m->fstype, (m->flags & MNT_RDONLY) ? "ro" : "rw");
    return n;
}

/* /proc/PID/mountinfo: id parent major:minor root mountpoint options - type source superoptions */
static size_t gen_mountinfo(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    int id = 20;
    for (struct mount *m = vfs_mounts(); m; m = m->next, id++) {
        const char *ro = (m->flags & MNT_RDONLY) ? "ro" : "rw";
        P("%d %d %u:%u / %s %s,relatime - %s %s %s\n", id, m->path[1] ? 20 : 1, (m->dev >> 8) & 0xfff,
          (m->dev & 0xff) | ((m->dev >> 12) & 0xfff00),
          m->path, ro, m->fstype, m->fstype, ro);
    }
    return n;
}

static size_t gen_filesystems(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("nodev\tramfs\nnodev\ttmpfs\nnodev\tdevfs\nnodev\tprocfs\nnodev\tsysfs\n\text2\n");
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
    n += msi_proc(buf + n, cap - n);
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
    for (int c = 0; c < ncpus; c++)                     /* the totals, shared out evenly */
        P("cpu%d %lu 0 %lu %lu 0 0 0 0 0 0\n", c, u / (uint64_t)ncpus, s / (uint64_t)ncpus, i / (uint64_t)ncpus);
    int last = 0;
    for (int k = 0; k < MAX_TASKS; k++) if (task_slot(k)->state != TASK_UNUSED && task_slot(k)->pid > last) last = task_slot(k)->pid;
    int64_t btime = time_realtime_sec() - (int64_t)(uptime_ms() / 1000);
    P("ctxt %lu\nbtime %ld\nprocesses %d\nprocs_running 1\nprocs_blocked 0\n", sched_context_switches(),
      btime, last);
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
static size_t gen_sound(char *buf, size_t cap, void *ctx) { (void)ctx; return hda_proc(buf, cap); }

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

/* ---- /proc/sys: read-only kernel settings ---------------------------------- */
#define SYSVAL(fn, ...) \
    static size_t fn(char *buf, size_t cap, void *ctx) { (void)ctx; size_t n = 0; P(__VA_ARGS__); return n; }
SYSVAL(gen_sys_ostype, "%s\n", KESTREL_NAME)
SYSVAL(gen_sys_osrelease, "%s\n", KESTREL_VERSION)
SYSVAL(gen_sys_hostname, "%s\n", KESTREL_HOSTNAME)
SYSVAL(gen_sys_pid_max, "%d\n", 32768)
SYSVAL(gen_sys_zero, "0\n")
SYSVAL(gen_sys_map_count, "65530\n")
SYSVAL(gen_sys_file_max, "%d\n", 65536)
SYSVAL(gen_sys_inotify_watches, "%d\n", 8192)
SYSVAL(gen_sys_inotify_inst, "%d\n", 128)
SYSVAL(gen_sys_inotify_queue, "%d\n", 16384)

/* A random UUID: boot_id is fixed for the boot, uuid differs on every read. */
static void make_uuid(char *out, uint64_t a, uint64_t b)
{
    uint8_t u[16];
    for (int i = 0; i < 8; i++) { u[i] = (uint8_t)(a >> (i * 8)); u[8 + i] = (uint8_t)(b >> (i * 8)); }
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return x ^ (x >> 33); }
static size_t gen_sys_boot_id(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    static uint64_t a, b;
    if (!a) { a = mix(rdtsc() ^ (uint64_t)time_realtime_sec()); b = mix(a ^ time_ns()); }
    char u[37]; make_uuid(u, a, b);
    P("%s\n", u);
    return n;
}
static size_t gen_sys_uuid(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    char u[37]; make_uuid(u, mix(rdtsc()), mix(time_ns() ^ 0x9e3779b97f4a7c15ull));
    P("%s\n", u);
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
    P("%d (%s) %c %d %d %d %d -1 %u 0 0 0 0 %lu 0 0 0 20 0 %d 0 %lu %lu %lu",
      t->pid, t->name, state_char(t), t->ppid, t->pgid, t->sid, tty, t->user ? 0u : 0x200000u,
      ticks, t->signal ? t->signal->nr_threads : 0, start, pages * 4096, pages);
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
      t->state <= TASK_ZOMBIE ? names[t->state] : "?", t->tgid, t->pid, t->ppid);
    P("Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\n", t->uid, t->uid, t->uid, t->uid,
      t->gid, t->gid, t->gid, t->gid);
    if (t->user) P("VmSize:\t%lu kB\nVmRSS:\t%lu kB\n", pages * 4, pages * 4);
    P("Threads:\t%d\nSigPnd:\t%016lx\nShdPnd:\t%016lx\nSigBlk:\t%016lx\n", t->signal ? t->signal->nr_threads : 0,
      t->sig_pending, t->signal ? t->signal->shared_pending : 0, t->sig_mask);
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

/* /proc/PID/statm: size resident shared text lib data dt, in pages */
static size_t gen_pid_statm(char *buf, size_t cap, void *ctx)
{
    size_t n = 0;
    struct tcb *t = pid_task(ctx);
    uint64_t pages = t && t->mm ? t->mm->rss : 0;
    P("%lu %lu 0 0 0 %lu 0\n", pages, pages, pages);
    return n;
}

static size_t gen_empty(char *buf, size_t cap, void *ctx) { (void)buf; (void)cap; (void)ctx; return 0; }

static size_t gen_pid_limits(char *buf, size_t cap, void *ctx)
{
    (void)ctx; size_t n = 0;
    P("Limit                     Soft Limit           Hard Limit           Units     \n");
    P("Max stack size            %-20lu %-20s bytes     \n", (uint64_t)UVM_STACK_SIZE, "unlimited");
    P("Max open files            %-20d %-20d files     \n", MAX_FDS, MAX_FDS);
    P("Max processes             %-20d %-20d processes \n", MAX_TASKS, MAX_TASKS);
    return n;
}

static size_t gen_oom_adj(char *buf, size_t cap, void *ctx) { (void)ctx; size_t n = 0; P("0\n"); return n; }

/* symbolic links whose target is computed: /proc/PID/exe, cwd, root */
static int link_exe(struct vnode *vn, char *buf, size_t size)
{
    struct tcb *t = pid_task(vn->ctx);
    if (!t || !t->exe[0]) return -ENOENT;
    size_t n = strlen(t->exe);
    if (n > size) n = size;
    memcpy(buf, t->exe, n);
    return (int)n;
}
static int link_cwd(struct vnode *vn, char *buf, size_t size)
{
    struct tcb *t = pid_task(vn->ctx);
    if (!t || !t->fs || !t->fs->cwd) return -ENOENT;
    char tmp[VFS_PATH_MAX];
    int r = vfs_path_of(t->fs->cwd, tmp, sizeof tmp);
    if (r < 0) return r;
    size_t n = strlen(tmp);
    if (n > size) n = size;
    memcpy(buf, tmp, n);
    return (int)n;
}
static int link_root(struct vnode *vn, char *buf, size_t size) { (void)vn; if (!size) return 0; buf[0] = '/'; return 1; }
static const struct vnode_ops exe_ops = { .readlink = link_exe };
static const struct vnode_ops cwd_ops = { .readlink = link_cwd };
static const struct vnode_ops root_ops = { .readlink = link_root };

static void proc_link(struct vnode *dir, const char *name, const struct vnode_ops *ops, void *ctx)
{
    struct vnode *l = vfs_node_new(dir->fs, name, VLNK, 0777, ops);
    if (!l) return;
    l->ctx = ctx;
    vfs_node_add(dir, l);
}

static size_t gen_kstat(char *buf, size_t cap, void *ctx) { (void)ctx; return kstat_gen(buf, cap); }
static size_t gen_kprof(char *buf, size_t cap, void *ctx) { (void)ctx; return kstat_gen_prof(buf, cap); }

static size_t gen_pid_kstat(char *buf, size_t cap, void *ctx)
{
    struct tcb *t = pid_task(ctx);
    return t ? kstat_gen_process(t->tgid, buf, cap) : 0;
}

static void pid_files(struct vnode *dir, void *ctx)
{
    pseudo_file(dir, "statm", gen_pid_statm, ctx);
    pseudo_file(dir, "mounts", gen_mounts, ctx);
    pseudo_file(dir, "mountinfo", gen_mountinfo, ctx);
    pseudo_file(dir, "environ", gen_empty, ctx);
    pseudo_file(dir, "limits", gen_pid_limits, ctx);
    pseudo_file(dir, "oom_score_adj", gen_oom_adj, ctx);
    pseudo_file(dir, "cgroup", gen_empty, ctx);
    proc_link(dir, "exe", &exe_ops, ctx);
    proc_link(dir, "cwd", &cwd_ops, ctx);
    proc_link(dir, "root", &root_ops, ctx);
    pseudo_file(dir, "stat", gen_pid_stat, ctx);
    pseudo_file(dir, "status", gen_pid_status, ctx);
    pseudo_file(dir, "cmdline", gen_pid_cmdline, ctx);
    pseudo_file(dir, "comm", gen_pid_comm, ctx);
    pseudo_file(dir, "maps", gen_pid_maps, ctx);
    pseudo_file(dir, "kstat", gen_pid_kstat, ctx);
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
        if (!t || t->state == TASK_UNUSED || t->pid != t->tgid) { detach(dir, d->dir); d->attached = false; }
    }
    for (int i = 0; i < MAX_TASKS; i++) {               /* new: attach a free entry */
        struct tcb *t = task_slot(i);
        if (t->state == TASK_UNUSED || t->pid != t->tgid) continue;  /* processes; not their threads */
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
            vfs_node_add(dir, d->dir);                  /* attach (at the end: getdents offsets) */
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
    pseudo_file(r, "kstat",       gen_kstat, NULL);
    pseudo_file(r, "kprof",       gen_kprof, NULL);
    pseudo_file(r, "pci",         gen_pci, NULL);
    pseudo_file(r, "usb",         gen_usb, NULL);
    pseudo_file(r, "partitions",  gen_partitions, NULL);
    pseudo_file(r, "bootvol",     gen_bootvol, NULL);
    pseudo_file(r, "sound",       gen_sound, NULL);
    struct vnode *sys = pseudo_dir(r, "sys");
    struct vnode *sk = pseudo_dir(sys, "kernel");
    pseudo_file(sk, "ostype",    gen_sys_ostype, NULL);
    pseudo_file(sk, "osrelease", gen_sys_osrelease, NULL);
    pseudo_file(sk, "hostname",  gen_sys_hostname, NULL);
    pseudo_file(sk, "pid_max",   gen_sys_pid_max, NULL);
    pseudo_file(sk, "threads-max", gen_sys_pid_max, NULL);
    pseudo_file(sk, "yama_ptrace_scope", gen_sys_zero, NULL);
    struct vnode *rnd = pseudo_dir(sk, "random");
    pseudo_file(rnd, "boot_id", gen_sys_boot_id, NULL);
    pseudo_file(rnd, "uuid",    gen_sys_uuid, NULL);
    struct vnode *svm = pseudo_dir(sys, "vm");
    pseudo_file(svm, "overcommit_memory", gen_sys_zero, NULL);
    pseudo_file(svm, "max_map_count", gen_sys_map_count, NULL);
    struct vnode *sfs = pseudo_dir(sys, "fs");
    pseudo_file(sfs, "file-max", gen_sys_file_max, NULL);
    struct vnode *ino = pseudo_dir(sfs, "inotify");
    pseudo_file(ino, "max_user_watches",   gen_sys_inotify_watches, NULL);
    pseudo_file(ino, "max_user_instances", gen_sys_inotify_inst, NULL);
    pseudo_file(ino, "max_queued_events",  gen_sys_inotify_queue, NULL);
    struct vnode *self = vfs_node_new(m, "self", VDIR, 0555, NULL);   /* the calling process */
    if (self) { vfs_node_add(r, self); pid_files(self, NULL); }
    proc_root = r;
    r->ops = &proc_root_ops;
    vfs_mount(m, mountpoint);
}
