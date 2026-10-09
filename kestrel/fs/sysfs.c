/* fs/sysfs.c -- /sys: one small attribute file per kernel object property */
#include <kernel/vfs.h>
#include <kernel/fb.h>
#include <kernel/task.h>
#include <kernel/arch.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>

/* A tiny attribute macro: each generator prints one value + newline. */
#define ATTR(fn, ...) \
    static size_t fn(char *buf, size_t cap, void *ctx) { (void)ctx; return (size_t)snprintf(buf, cap, __VA_ARGS__); }

ATTR(a_ostype,     "%s\n", KESTREL_NAME)
ATTR(a_osrelease,  "%s\n", KESTREL_VERSION)
ATTR(a_hostname,   "%s\n", KESTREL_HOSTNAME)
ATTR(a_fb_name,    "%s\n", g_boot.uefi ? "efi-gop" : "vesa-vbe")
ATTR(a_fb_size,    "%u,%u\n", g_fb.width, g_fb.height)
ATTR(a_fb_bpp,     "%u\n", g_fb.bpp)
ATTR(a_fb_stride,  "%u\n", g_fb.pitch)
ATTR(a_fb_phys,    "0x%lx\n", g_fb.phys)
ATTR(a_fb_dbl,     "%d\n", g_fb.double_buffered ? 1 : 0)
ATTR(a_loader,     "%s\n", g_boot.loader)
ATTR(a_bootmode,   "%s\n", g_boot.uefi ? "uefi" : "bios")
ATTR(a_bootentry,  "%s\n", g_boot.boot_entry_name)
ATTR(a_bootflags,  "0x%x\n", g_boot.boot_flags)
ATTR(a_cpu_vendor, "%s\n", g_boot.cpu_vendor)
ATTR(a_cpu_model,  "%s\n", g_boot.cpu_brand)
ATTR(a_preempt,    "%d\n", sched_preemption() ? 1 : 0)
ATTR(a_quantum,    "%d\n", SCHED_QUANTUM)
ATTR(a_ctxsw,      "%lu\n", sched_context_switches())
ATTR(a_hz,         "%d\n", PIT_HZ)
ATTR(a_pwr_state,  "freeze mem disk\n")

void sysfs_init(const char *mountpoint)
{
    struct mount *m;
    struct vnode *r = vfs_mkfs("sysfs", MNT_RDONLY | MNT_NOCREATE, &m);

    struct vnode *kernel = pseudo_dir(r, "kernel");
    pseudo_file(kernel, "ostype",    a_ostype, NULL);
    pseudo_file(kernel, "osrelease", a_osrelease, NULL);
    pseudo_file(kernel, "hostname",  a_hostname, NULL);
    struct vnode *sched = pseudo_dir(kernel, "sched");
    pseudo_file(sched, "preemption",       a_preempt, NULL);
    pseudo_file(sched, "quantum_ms",       a_quantum, NULL);
    pseudo_file(sched, "context_switches", a_ctxsw, NULL);
    pseudo_file(sched, "tick_hz",          a_hz, NULL);

    struct vnode *cls = pseudo_dir(r, "class");
    struct vnode *gfx = pseudo_dir(cls, "graphics");
    struct vnode *fb0 = pseudo_dir(gfx, "fb0");
    pseudo_file(fb0, "name",            a_fb_name, NULL);
    pseudo_file(fb0, "virtual_size",    a_fb_size, NULL);
    pseudo_file(fb0, "bits_per_pixel",  a_fb_bpp, NULL);
    pseudo_file(fb0, "stride",          a_fb_stride, NULL);
    pseudo_file(fb0, "phys_addr",       a_fb_phys, NULL);
    pseudo_file(fb0, "double_buffered", a_fb_dbl, NULL);

    struct vnode *fw = pseudo_dir(r, "firmware");
    pseudo_file(fw, "bootloader", a_loader, NULL);
    pseudo_file(fw, "boot_mode",  a_bootmode, NULL);
    pseudo_file(fw, "boot_entry", a_bootentry, NULL);
    pseudo_file(fw, "boot_flags", a_bootflags, NULL);

    struct vnode *dev = pseudo_dir(r, "devices");
    struct vnode *sys = pseudo_dir(dev, "system");
    struct vnode *cpu = pseudo_dir(sys, "cpu");
    struct vnode *cpu0 = pseudo_dir(cpu, "cpu0");
    pseudo_file(cpu0, "vendor",     a_cpu_vendor, NULL);
    pseudo_file(cpu0, "model_name", a_cpu_model, NULL);

    struct vnode *pwr = pseudo_dir(r, "power");
    pseudo_file(pwr, "state", a_pwr_state, NULL);

    vfs_mount(m, mountpoint);
}
