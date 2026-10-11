/* kernel/vdso.c -- the vDSO's pages and their place in every process (see vdso.h)
 *
 * One vm_object holds both: page 0 is the data page (struct vdso_data),
 * the rest the ELF image linked from vdso/vdso.c and embedded in the
 * kernel (arch/x86_64/vdso_blob.asm). exec maps the data page read-only
 * and the image read-and-execute at fixed addresses; fork copies the
 * mappings like any shared mapping, and mprotect refuses to make either
 * writable. kestrel.vdso=0 leaves it out. */
#include <kernel/vdso.h>
#include <kernel/vmobj.h>
#include <kernel/vm.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>

#define MSR_TSC_AUX 0xC0000103

extern const uint8_t vdso_image[], vdso_image_end[];

static struct vm_object *obj;
static struct vdso_data *data;
static uint64_t image_pages;
static uint32_t cpu_mode;

static uint32_t detect_cpu_mode(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        if (c & (1u << 22)) return VDSO_CPU_RDPID;
    }
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 27)) return VDSO_CPU_RDTSCP;
    }
    return VDSO_CPU_SYSCALL;
}

/* Each CPU: IA32_TSC_AUX = its number, which RDPID and RDTSCP return. */
void vdso_cpu_init(void)
{
    if (detect_cpu_mode() != VDSO_CPU_SYSCALL) wrmsr(MSR_TSC_AUX, (uint64_t)this_cpu()->id);
}

void vdso_update(void)
{
    if (!data) return;
    uint64_t start = 0, hz = 0;
    bool tsc = time_tsc_clock(&start, &hz);
    __atomic_add_fetch(&data->seq, 1, __ATOMIC_SEQ_CST);        /* odd: readers retry */
    data->clock_mode = tsc ? VDSO_CLOCK_TSC : VDSO_CLOCK_SYSCALL;
    data->tsc_start = start;
    data->tsc_hz = hz;
    data->boot_epoch = time_boot_epoch();
    data->cpu_mode = cpu_mode;
    __atomic_add_fetch(&data->seq, 1, __ATOMIC_SEQ_CST);
}

void vdso_init(void)
{
    if (strstr(g_boot.cmdline, "kestrel.vdso=0")) { kprintf("vdso: off (kestrel.vdso=0)\n"); return; }
    size_t len = (size_t)(vdso_image_end - vdso_image);
    image_pages = (len + 4095) / 4096;
    obj = vmobj_new((1 + image_pages) * 4096);
    if (!obj) return;
    uint64_t phys = vmobj_page(obj, 0, true);
    if (!phys || vmobj_write(obj, vdso_image, len, 4096) != (ssize_t)len) {
        vmobj_put(obj);
        obj = NULL;
        kprintf("vdso: out of memory\n");
        return;
    }
    data = (struct vdso_data *)(uintptr_t)phys;                 /* identity-mapped */
    cpu_mode = detect_cpu_mode();
    vdso_update();
    static const char *const cpu_names[] = { "system call", "RDPID", "RDTSCP" };
    kprintf("vdso: %lu bytes; clock %s, getcpu by %s\n", (unsigned long)len,
            data->clock_mode == VDSO_CLOCK_TSC ? "from the TSC" : "by system call", cpu_names[cpu_mode]);
}

uint64_t vdso_map(struct mm *mm)
{
    if (!obj) return 0;
    if (vm_mmap(mm, VDSO_VVAR_ADDR, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, NULL, obj, 0, 0) < 0) return 0;
    vm_mark(mm, VDSO_VVAR_ADDR, VMA_VVAR);
    if (vm_mmap(mm, VDSO_ADDR, image_pages * 4096, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED,
                NULL, obj, 4096, 0) < 0) {
        vm_munmap(mm, VDSO_VVAR_ADDR, 4096);
        return 0;
    }
    vm_mark(mm, VDSO_ADDR, VMA_VDSO);
    return VDSO_ADDR;
}
