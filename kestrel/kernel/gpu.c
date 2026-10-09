/* kernel/gpu.c -- glue between the PCI enumerator, the NVIDIA probe
 * (drivers/gpu/nvidia) and the rest of Kestrel.
 *
 * Stage 1 only detects and reports; the monitor that the firmware lit stays
 * monitor 0 (the boot frame buffer). The report lands in REPORT.TXT and the
 * card's video BIOS in VBIOS.ROM on the boot stick (report.c), and the
 * shell command 'gpu' prints it. Boot with nvidia.probe=0 to skip it. */
#include <kernel/gpu.h>
#include <kernel/report.h>
#include <kernel/klog.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/time.h>
#include <kernel/bootinfo.h>
#include <kernel/cpu.h>
#include <kernel/display.h>
#include "pci.h"
#include "nvidia.h"

#define MAX_GPUS 2

static struct nv_device gpus[MAX_GPUS];
static int ngpus;

/* ---- platform hooks --------------------------------------------------------- */
static void *map_mmio(uint64_t phys, size_t size)
{
    if (phys + size > PMM_MAX_PHYS && !vmm_identity_map(phys, size, VMM_WRITE)) return NULL;
    if (!vmm_set_uncached(phys, size)) return NULL;
    return (void *)(uintptr_t)phys;
}

static void *alloc(size_t size)
{
    uint64_t pa = pmm_alloc_contig((size + PAGE_SIZE - 1) / PAGE_SIZE);   /* zeroed, identity mapped */
    return pa ? (void *)(uintptr_t)pa : NULL;
}

static void release(void *p, size_t size)
{
    if (p) pmm_free_contig((uint64_t)(uintptr_t)p, (size + PAGE_SIZE - 1) / PAGE_SIZE);
}

/* The PCI expansion ROM BAR (config 0x30): size it, enable decoding, copy,
 * then restore the register. Only used when PRAMIN and PROM both failed. */
static struct pci_device *rom_dev;
static uint32_t read_pci_rom(void *buf, uint32_t max)
{
    struct pci_device *p = rom_dev;
    uint32_t orig = pci_read32(p->bus, p->device, p->function, 0x30);
    uint64_t base = orig & 0xfffff800u;
    if (!base) { kprintf("nvidia: expansion ROM BAR not assigned by the firmware\n"); return 0; }
    pci_write32(p->bus, p->device, p->function, 0x30, 0xfffff800u);
    uint32_t sz = ~(pci_read32(p->bus, p->device, p->function, 0x30) & 0xfffff800u) + 1;
    pci_write32(p->bus, p->device, p->function, 0x30, (uint32_t)base | 1);     /* enable decoding */
    uint32_t n = sz < max ? sz : max, copied = 0;
    if (map_mmio(base, n)) {
        const volatile uint32_t *rom = (const volatile uint32_t *)(uintptr_t)base;
        for (uint32_t i = 0; i < n / 4; i++) ((uint32_t *)buf)[i] = rom[i];
        copied = n;
    }
    pci_write32(p->bus, p->device, p->function, 0x30, orig);
    return copied;
}

/* Modeset milestones: rewrite REPORT.TXT so the log survives a hang or a
 * dark screen (there is no serial port on the test machine). */
static bool report_ready;
static void checkpoint(const char *stage)
{
    if (report_ready) report_save(stage);
}

static const struct nv_platform kestrel_nv = {
    .checkpoint   = checkpoint,
    .map_mmio     = map_mmio,
    .delay_us     = udelay,
    .log_putc     = klog_putc,
    .alloc        = alloc,
    .free         = release,
    .read_pci_rom = read_pci_rom,
};

/* ---- stage 2: modeset (nvidia.modeset=1, or =2 to also re-drive firmware-lit
 * monitors) ----------------------------------------------------------------- */
static int modeset_level(void)
{
    const char *o = strstr(g_boot.cmdline, "nvidia.modeset=");
    return o ? o[15] - '0' : 0;
}

static void try_modeset(struct pci_device *p, struct nv_device *d)
{
    int level = modeset_level();
    if (level != 1 && level != 2) return;
    /* Supervisor interrupts are polled; keep the GPU's INTx line quiet. */
    uint16_t cmd = pci_read16(p->bus, p->device, p->function, 0x04);
    pci_write16(p->bus, p->device, p->function, 0x04, cmd | PCI_CMD_INTX_DISABLE);
    report_ready = !strstr(g_boot.cmdline, "kestrel.report=0") && report_locate();
    struct nv_modeset_opts opts = { .trace_scripts = true, .takeover = level == 2 };
    const char *w = strstr(g_boot.cmdline, "nvidia.maxres=");
    if (w) {
        w += 14;
        while (*w >= '0' && *w <= '9') opts.max_width = opts.max_width * 10 + (*w++ - '0');
        if (*w == 'x') for (w++; *w >= '0' && *w <= '9'; w++) opts.max_height = opts.max_height * 10 + (*w - '0');
    }
    kprintf("nvidia: experimental modeset (nvidia.modeset=%d)\n", level);
    uint64_t t0 = time_ms();
    int rc = nv_modeset(d, &opts);
    kprintf("nvidia: modeset finished in %lu ms: %s\n", time_ms() - t0, nv_strerror(rc));
}

struct nv_display_ctx { struct nv_device *dev; uint64_t fb; };

/* Frame buffers in VRAM are written through the BAR0 PRAMIN window, which
 * is one shared register: keep interrupts (and so other TTYs) out. */
static void vram_span(void *ctx, uint32_t offset, const void *src, uint32_t len)
{
    struct nv_display_ctx *c = ctx;
    uint64_t f = irq_save();
    nv_vram_write(c->dev, c->fb + offset, src, len);
    irq_restore(f);
}

void gpu_register_displays(void)
{
    static struct nv_display_ctx ctxs[MAX_GPUS * NV_MAX_HEADS];
    int nctx = 0;
    for (int g = 0; g < ngpus; g++) {
        struct nv_device *d = &gpus[g];
        for (int i = 0; i < d->ms.nouts; i++) {
            const struct nv_output *o = &d->ms.out[i];
            if (o->kept || !o->lit) continue;
            size_t bytes = (size_t)o->pitch * o->mode.vdisplay;
            uint64_t shadow = pmm_alloc_contig((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
            if (!shadow) { kprintf("nvidia: no RAM for the shadow of head %d\n", o->head); continue; }
            struct nv_display_ctx *c = &ctxs[nctx++];
            c->dev = d;
            c->fb = o->fb;
            struct display_head h = {
                .phys = 0, .vram = NULL, .shadow = (uint32_t *)(uintptr_t)shadow,
                .width = o->mode.hdisplay, .height = o->mode.vdisplay, .pitch = (int)o->pitch, .bytes_pp = 4,
                .r_pos = 16, .g_pos = 8, .b_pos = 0,
                .write_span = vram_span, .write_ctx = c,
            };
            snprintf(h.name, sizeof h.name, "%s head %d DP-%d", d->chip_name, o->head, o->dcb);
            display_register(&h);
        }
    }
}

/* ---- probe ------------------------------------------------------------------ */
static size_t report_section(char *buf, size_t cap) { return gpu_report(buf, cap); }

void gpu_probe(void)
{
    if (strstr(g_boot.cmdline, "nvidia.probe=0")) { kprintf("nvidia: probing disabled (nvidia.probe=0)\n"); return; }
    for (size_t i = 0; i < pci_device_count() && ngpus < MAX_GPUS; i++) {
        struct pci_device *p = pci_device_at(i);
        if (p->vendor_id != PCI_VENDOR_NVIDIA || p->class_code != PCI_CLASS_DISPLAY) continue;
        const struct pci_bar *b0 = &p->bars[0], *b1 = &p->bars[1];
        if (b0->kind != PCI_BAR_MEM32 && b0->kind != PCI_BAR_MEM64) {
            kprintf("nvidia: %02x:%02x.%x has no memory BAR0, skipped\n", p->bus, p->device, p->function);
            continue;
        }
        struct nv_device *d = &gpus[ngpus++];
        memset(d, 0, sizeof *d);
        d->bus = p->bus; d->dev = p->device; d->fn = p->function;
        d->vendor_id = p->vendor_id; d->device_id = p->device_id;
        d->subsys_vendor = p->subsystem_vendor_id; d->subsys_id = p->subsystem_id;
        d->bar0 = b0->base;
        if (b1->kind == PCI_BAR_MEM32 || b1->kind == PCI_BAR_MEM64) { d->bar1 = b1->base; d->bar1_size = b1->size; }

        pci_enable(p, false, true, false);          /* MMIO decoding (normally already on) */
        rom_dev = p;
        uint64_t t0 = time_ms();
        int rc = nv_probe(d, &kestrel_nv);
        kprintf("nvidia: probe of %02x:%02x.%x finished in %lu ms: %s\n", p->bus, p->device, p->function,
                time_ms() - t0, nv_strerror(rc));
        if (d->bios_data && d->bios_size) report_set_blob(d->bios_data, d->bios_size, "NVIDIA video BIOS");
        if (rc == NV_OK) try_modeset(p, d);
    }
    if (ngpus) report_add_section("NVIDIA GPU", report_section);
}

int gpu_count(void) { return ngpus; }

size_t gpu_report(char *buf, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < ngpus && n < cap; i++) {
        if (ngpus > 1) n += (size_t)snprintf(buf + n, cap - n, "--- GPU %d ---\n", i);
        if (n < cap) n += nv_report(&gpus[i], buf + n, cap - n);
    }
    if (!ngpus && cap) n = (size_t)snprintf(buf, cap, "no NVIDIA display controller found\n");
    return n < cap ? n : cap;
}
