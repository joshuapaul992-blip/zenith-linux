/* kernel/display.c -- display head registry and boot-time probing
 *
 *   head 0     the boot frame buffer (VBE or GOP mode chosen by GRUB), drawn
 *              through its off-screen copy when double buffering is on
 *   head 1..   every secondary display adapter a driver can program; today
 *              QEMU/Bochs DISPI adapters (bochs-display, secondary-vga),
 *              each set to its monitor's EDID-preferred resolution
 *
 * A native GPU driver adds its monitors the same way, through
 * display_register(). */
#include <kernel/display.h>
#include <kernel/fb.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/gpu.h>
#include "pci.h"
#include "edid.h"
#include "bochs_dispi.h"

#define DISPI_FALLBACK_W    1024        /* no EDID: a mode every monitor accepts */
#define DISPI_FALLBACK_H    768

static struct display_head heads[MAX_MONITORS];
static int nheads;

int display_register(const struct display_head *h)
{
    if (nheads >= MAX_MONITORS) {
        kprintf("display: %s ignored, %d monitors already registered\n", h->name, MAX_MONITORS);
        return -1;
    }
    if ((!h->vram && !(h->write_span && h->shadow)) || h->width <= 0 || h->height <= 0 || (h->bytes_pp != 3 && h->bytes_pp != 4) ||
        h->pitch < h->width * h->bytes_pp) {
        kprintf("display: %s ignored, bad geometry %dx%d pitch %d\n", h->name, h->width, h->height, h->pitch);
        return -1;
    }
    heads[nheads] = *h;
    kprintf("display: monitor %d: %dx%dx%d, pitch %d, VRAM %lx%s, %s%s%s%s\n", nheads, h->width, h->height,
            h->bytes_pp * 8, h->pitch, h->phys, h->shadow ? " + RAM shadow" : "", h->name,
            h->monitor[0] ? " (\"" : "", h->monitor, h->monitor[0] ? "\")" : "");
    return nheads++;
}

int display_count(void) { return nheads; }

const struct display_head *display_get(int index)
{
    return index >= 0 && index < nheads ? &heads[index] : NULL;
}

/* ---- head 0: the boot frame buffer ---------------------------------------- */
static void probe_boot_fb(void)
{
    if (!g_fb.ready) return;
    if (gpu_replaced_boot_fb(g_fb.phys)) {          /* nvidia.modeset=2: not on screen any more */
        kprintf("display: boot frame buffer replaced by the NVIDIA modeset, no terminal on it\n");
        return;
    }
    if ((g_fb.bpp != 32 && g_fb.bpp != 24) || g_fb.r_size != 8 || g_fb.g_size != 8 || g_fb.b_size != 8) {
        kprintf("display: boot frame buffer is %u bpp (%u:%u:%u), no text terminal on it\n",
                g_fb.bpp, g_fb.r_size, g_fb.g_size, g_fb.b_size);
        return;
    }
    struct display_head h = {
        .phys   = g_fb.phys,
        .vram   = (uint32_t *)g_fb.front,
        .shadow = g_fb.double_buffered ? (uint32_t *)g_fb.back : NULL,
        .width  = (int)g_fb.width,
        .height = (int)g_fb.height,
        .pitch  = (int)g_fb.pitch,
        .bytes_pp = (int)g_fb.bytes_pp,
        .r_pos  = g_fb.r_pos, .g_pos = g_fb.g_pos, .b_pos = g_fb.b_pos,
    };
    strlcpy(h.name, g_fb.phys ? "boot frame buffer" : "frame buffer", sizeof h.name);
    display_register(&h);
}

/* ---- secondary heads: Bochs/QEMU DISPI adapters --------------------------- */
static void probe_dispi(struct pci_device *dev)
{
    const struct pci_bar *lfb = &dev->bars[0], *mmio_bar = &dev->bars[2];
    if ((lfb->kind != PCI_BAR_MEM32 && lfb->kind != PCI_BAR_MEM64) || !lfb->base || !lfb->size) return;
    /* The adapter GRUB already drives is head 0. */
    if (g_fb.phys >= lfb->base && g_fb.phys < lfb->base + lfb->size) return;
    if ((mmio_bar->kind != PCI_BAR_MEM32 && mmio_bar->kind != PCI_BAR_MEM64) || !mmio_bar->base) {
        kprintf("display: %02x:%02x.%x has no MMIO BAR2 (ISA-style VGA), skipped\n",
                dev->bus, dev->device, dev->function);
        return;
    }

    pci_enable(dev, false, true, false);
    if (!vmm_set_uncached(mmio_bar->base, 4096)) return;
    if (lfb->base + lfb->size > PMM_MAX_PHYS && !vmm_identity_map(lfb->base, lfb->size, VMM_WRITE)) return;
    volatile void *mmio = (volatile void *)(uintptr_t)mmio_bar->base;
    if (!dispi_present(mmio)) {
        kprintf("display: %02x:%02x.%x: no DISPI interface\n", dev->bus, dev->device, dev->function);
        return;
    }

    uint8_t raw[EDID_BLOCK_SIZE];
    struct edid_info edid;
    dispi_read_edid(mmio, raw);
    int w = DISPI_FALLBACK_W, h = DISPI_FALLBACK_H;
    if (edid_parse(raw, &edid)) {
        kprintf("display: %02x:%02x.%x: EDID %u.%u %s %04x \"%s\", native %dx%d @ %d Hz, %dx%d mm\n",
                dev->bus, dev->device, dev->function, edid.version, edid.revision, edid.vendor,
                edid.product, edid.name, edid.native_width, edid.native_height, edid.refresh_hz,
                edid.width_mm, edid.height_mm);
        w = edid.native_width;
        h = edid.native_height;
    } else {
        kprintf("display: %02x:%02x.%x: no valid EDID, using %dx%d\n",
                dev->bus, dev->device, dev->function, w, h);
    }

    struct dispi_mode mode;
    if (!dispi_set_mode(mmio, w, h, &mode) || (uint64_t)mode.pitch * (uint64_t)mode.height > lfb->size) {
        kprintf("display: %02x:%02x.%x: cannot set %dx%dx32\n", dev->bus, dev->device, dev->function, w, h);
        return;
    }

    struct display_head head = {
        .phys = lfb->base, .vram = (uint32_t *)(uintptr_t)lfb->base,
        .width = mode.width, .height = mode.height, .pitch = mode.pitch, .bytes_pp = 4,
        .r_pos = 16, .g_pos = 8, .b_pos = 0,                /* x8r8g8b8 */
    };
    size_t frames = ((size_t)mode.pitch * (size_t)mode.height + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t shadow = pmm_alloc_contig(frames);             /* zeroed, identity mapped */
    head.shadow = (uint32_t *)(uintptr_t)shadow;
    snprintf(head.name, sizeof head.name, "DISPI adapter %02x:%02x.%x", dev->bus, dev->device, dev->function);
    strlcpy(head.monitor, edid.name, sizeof head.monitor);
    if (display_register(&head) < 0 && shadow) pmm_free_contig(shadow, frames);
}

void display_probe(void)
{
    gpu_probe();                    /* native GPU drivers: NVIDIA detection (stage 1) */
    probe_boot_fb();
    gpu_register_displays();        /* monitors lit by nvidia.modeset=1 */
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_device *dev = pci_device_at(i);
        if (dev->vendor_id == DISPI_PCI_VENDOR && dev->device_id == DISPI_PCI_DEVICE &&
            dev->class_code == PCI_CLASS_DISPLAY)
            probe_dispi(dev);
    }
    kprintf("display: %d monitor(s)\n", nheads);
}
