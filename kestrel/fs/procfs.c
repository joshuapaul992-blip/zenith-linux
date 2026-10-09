/* fs/procfs.c -- /proc: process and kernel state, generated on every read */
#include <kernel/vfs.h>
#include <kernel/task.h>
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

void procfs_init(const char *mountpoint)
{
    struct mount *m;
    struct vnode *r = vfs_mkfs("procfs", MNT_RDONLY | MNT_NOCREATE, &m);
    pseudo_file(r, "version",     gen_version, NULL);
    pseudo_file(r, "uptime",      gen_uptime, NULL);
    pseudo_file(r, "meminfo",     gen_meminfo, NULL);
    pseudo_file(r, "stat",        gen_stat, NULL);
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
    vfs_mount(m, mountpoint);
}
