/* linuxkpi/glue/kestrel.c -- the Linux KPI's view of Kestrel (lkpi/glue.h)
 *
 * Everything the Linux side needs from the kernel goes through here:
 * memory, the clock, threads and their sleep/wake protocol, PCI, MSI and
 * files. This file is ordinary Kestrel code (Kestrel headers, Kestrel
 * semantics); it is the one place where both worlds meet. */
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/task.h>
#include <kernel/smp.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>
#include <kernel/process.h>
#include <kernel/uaccess.h>
#include <kernel/power.h>
#include <kernel/msi.h>
#include <kernel/vfs.h>
#include <kernel/posix.h>
#include "pci.h"
#include "../include/lkpi/glue.h"

/* ---- memory ---------------------------------------------------------------- */
void *lkpi_heap_alloc(unsigned long n) { return kmalloc(n); }
void  lkpi_heap_free(void *p) { kfree(p); }

unsigned long lkpi_frames_alloc(unsigned long n, int dma32)
{
    if (!n) return 0;
    if (dma32) return pmm_alloc_dma32(n);
    return n == 1 ? pmm_alloc() : pmm_alloc_contig(n);
}

void lkpi_frames_free(unsigned long phys, unsigned long n)
{
    if (n == 1) pmm_free(phys);
    else pmm_free_contig(phys, n);
}

unsigned long lkpi_ram_total(void) { return pmm_total_bytes(); }
unsigned long lkpi_ram_free(void) { return pmm_free_bytes(); }

int lkpi_map_mmio(unsigned long phys, unsigned long size, int wc)
{
    if (wc && vmm_set_write_combining(phys, size)) return 0;
    return vmm_set_uncached(phys, size) ? 0 : -1;
}

/* ---- time ------------------------------------------------------------------ */
unsigned long long lkpi_time_ns(void) { return time_ns(); }
unsigned long long lkpi_time_ms(void) { return time_ms(); }
void lkpi_udelay(unsigned int us) { udelay(us); }

long long lkpi_realtime_ns(void)
{
    int64_t s, ns;
    time_realtime(&s, &ns);
    return s * 1000000000ll + ns;
}

/* ---- CPUs and threads -------------------------------------------------------- */
int lkpi_cpu_id(void) { return this_cpu()->id; }
int lkpi_cpu_count(void) { return ncpus; }
void *lkpi_task_self(void) { return current_task(); }
int lkpi_task_slots(void) { return MAX_TASKS; }

int lkpi_task_slot(void *task)
{
    struct tcb *t = task, *first = task_slot(0);
    if (t < first || t > task_slot(MAX_TASKS - 1)) return -1;     /* an idle thread */
    return (int)(t - first);
}

int lkpi_task_pid(void *task) { return ((struct tcb *)task)->pid; }
const char *lkpi_task_name(void *task) { return ((struct tcb *)task)->name; }
int lkpi_task_is_user(void *task) { return ((struct tcb *)task)->user; }
void *lkpi_task_create(const char *name, int (*fn)(void *), void *arg) { return task_create(name, fn, arg); }
void lkpi_task_exit(int code) { task_exit(code); }
void lkpi_task_block(void *chan, unsigned long long deadline_ms)
{
    if (current_task()->pid != 0) task_block_self(chan, deadline_ms);     /* see lkpi_task_switch */
}
void lkpi_task_wake(void *task) { task_wake(task); }
void lkpi_task_yield(void) { sched_yield(); }
int lkpi_task_signal_pending(void) { return current_task()->user && signal_pending(); }

void lkpi_task_switch(void)
{
    struct tcb *self = current_task();
    if (self->pid == 0) {                       /* the boot thread cannot block: just wait a tick */
        self->wait_chan = NULL;
        sti(); hlt(); cli();
        return;
    }
    schedule();
    self->wait_chan = NULL;
}

/* The boot thread (pid 0) only runs when nothing else is ready, and a
 * thread that becomes ready does not preempt it: yield explicitly, and
 * halt until the next interrupt when there was nothing to yield to. */
void lkpi_boot_wait(volatile int *flag)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        sched_yield();
        uint64_t f = irq_save();
        if (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) { sti(); hlt(); cli(); }
        irq_restore(f);
    }
}

/* ---- logging, boot parameters, hooks ------------------------------------------ */
void lkpi_log(const char *s) { kprintf("%s", s); }
const char *lkpi_cmdline(void) { return g_boot.cmdline; }

void lkpi_set_hooks(void (*panic_fn)(const char *msg), void (*power_fn)(int what))
{
    panic_set_hook(panic_fn);
    power_set_hook(power_fn);
}

void lkpi_die(const char *msg) { panic("%s", msg); }

void lkpi_power(int what)
{
    if (what == POWER_HOOK_REBOOT) power_reboot();
    if (what == POWER_HOOK_HALT) power_halt();
    power_off();
}

/* ---- user memory ---------------------------------------------------------- */
int lkpi_copy_from_user_glue(void *dst, const void *usrc, unsigned long n) { return copy_from_user(dst, usrc, n); }
int lkpi_copy_to_user_glue(void *udst, const void *src, unsigned long n) { return copy_to_user(udst, src, n); }
int lkpi_user_range_ok(unsigned long addr, unsigned long n) { return user_range_ok(addr, n); }

/* ---- PCI ----------------------------------------------------------------- */
int lkpi_pci_count(void) { return (int)pci_device_count(); }

int lkpi_pci_get(int index, struct lkpi_pci_info *out)
{
    struct pci_device *d = pci_device_at((size_t)index);
    if (!d) return -1;
    memset(out, 0, sizeof *out);
    out->handle = d;
    out->bus = d->bus; out->dev = d->device; out->fn = d->function;
    out->vendor = d->vendor_id; out->device = d->device_id;
    out->subvendor = d->subsystem_vendor_id; out->subdevice = d->subsystem_id;
    out->class_code = d->class_code; out->subclass = d->subclass;
    out->prog_if = d->prog_if; out->revision = d->revision;
    out->irq_line = d->interrupt_line; out->irq_pin = d->interrupt_pin;
    out->header_type = d->header_type;
    for (int i = 0; i < d->bar_count && i < 6; i++) {
        out->bar[i].base = d->bars[i].base;
        out->bar[i].size = d->bars[i].size;
        out->bar[i].kind = d->bars[i].kind;
        out->bar[i].prefetch = d->bars[i].prefetchable;
    }
    out->parent = d->parent;
    return 0;
}

unsigned int lkpi_pci_read(void *handle, int off, int size)
{
    struct pci_device *d = handle;
    if (off < 0 || off + size > 256) return ~0u;
    switch (size) {
    case 1:  return pci_read8(d->bus, d->device, d->function, (uint8_t)off);
    case 2:  return pci_read16(d->bus, d->device, d->function, (uint8_t)off);
    default: return pci_read32(d->bus, d->device, d->function, (uint8_t)off);
    }
}

/* Narrow writes are real 8- and 16-bit configuration cycles, not a
 * read-modify-write of the dword: devices decode the access size (QEMU's
 * i6300esb takes its lock register only as a byte), and rewriting the
 * neighbouring bytes would clear write-1-to-clear bits such as Status. */
void lkpi_pci_write(void *handle, int off, int size, unsigned int v)
{
    struct pci_device *d = handle;
    if (off < 0 || off + size > 256) return;
    if (size == 4) { pci_write32(d->bus, d->device, d->function, (uint8_t)off, v); return; }
    uint32_t addr = 0x80000000u | ((uint32_t)d->bus << 16) | ((uint32_t)(d->device & 0x1F) << 11) |
                    ((uint32_t)(d->function & 0x07) << 8) | ((uint32_t)off & 0xFC);
    uint64_t f = irq_save();
    outl(0xCF8, addr);
    if (size == 1) outb((uint16_t)(0xCFC + (off & 3)), (uint8_t)v);
    else outw((uint16_t)(0xCFC + (off & 2)), (uint16_t)v);
    irq_restore(f);
}

int lkpi_pci_msi_enable(void *handle, void (*fn)(void *), void *arg, const char *name)
{
    return msi_enable(handle, fn, arg, name);
}

void lkpi_pci_msi_disable(void *handle, int vector) { msi_disable(handle, vector); }
int lkpi_pci_msi_is_msix(int vector) { return msi_is_msix(vector); }

/* ---- files ------------------------------------------------------------------ */
long lkpi_file_read_all(const char *path, void **data, unsigned long max)
{
    struct stat st;
    int r = vfs_stat(path, &st);
    if (r < 0) return r;
    if (!S_ISREG(st.st_mode)) return -EINVAL;
    if ((unsigned long)st.st_size > max) return -EFBIG;
    struct file *f;
    if ((r = vfs_open(path, O_RDONLY, 0, &f)) < 0) return r;
    size_t size = (size_t)st.st_size;
    uint8_t *buf = kmalloc(size ? size : 1);
    if (!buf) { vfs_close(f); return -ENOMEM; }
    size_t got = 0;
    while (got < size) {
        ssize_t n = vfs_read(f, buf + got, size - got);
        if (n < 0) { kfree(buf); vfs_close(f); return n; }
        if (n == 0) break;
        got += (size_t)n;
    }
    vfs_close(f);
    *data = buf;
    return (long)got;
}
