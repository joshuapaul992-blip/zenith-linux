/* kernel/msi.c -- message-signalled interrupts for PCI devices (see msi.h) */
#include <kernel/msi.h>
#include <kernel/smp.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include "pci.h"

#define CAP_MSI         0x05
#define CAP_MSIX        0x11
#define MSI_ADDR_BASE   0xFEE00000u             /* the local APICs' window */

struct msi_slot {
    msi_handler_t fn;
    void         *arg;
    const char   *name;
    uint64_t      count;
    bool          msix;
};
static struct msi_slot slots[VEC_MSI_COUNT];

/* Offset of capability `id` in the function's list, 0 if absent. */
static uint8_t find_cap(const struct pci_device *d, uint8_t id)
{
    if (!(pci_read16(d->bus, d->device, d->function, PCI_REG_STATUS) & PCI_STATUS_CAP_LIST)) return 0;
    uint8_t off = pci_read8(d->bus, d->device, d->function, PCI_REG_CAP_PTR) & 0xFC;
    for (int guard = 0; off && guard < 48; guard++) {
        if (pci_read8(d->bus, d->device, d->function, off) == id) return off;
        off = pci_read8(d->bus, d->device, d->function, (uint8_t)(off + 1)) & 0xFC;
    }
    return 0;
}

/* MSI-X: entry 0 of the table (in one of the BARs) gets the message. */
static bool setup_msix(const struct pci_device *d, uint8_t cap, uint32_t addr, uint32_t data)
{
    uint32_t tbl = pci_read32(d->bus, d->device, d->function, (uint8_t)(cap + 4));
    unsigned bir = tbl & 7;
    if (bir >= d->bar_count) return false;
    const struct pci_bar *b = &d->bars[bir];
    if ((b->kind != PCI_BAR_MEM32 && b->kind != PCI_BAR_MEM64) || !b->base) return false;
    uint64_t entry = b->base + (tbl & ~7u);
    vmm_set_uncached(entry & ~0xFFFull, 4096);
    uint16_t ctl = pci_read16(d->bus, d->device, d->function, (uint8_t)(cap + 2));
    /* enabled with every vector masked while the entry is written */
    pci_write16(d->bus, d->device, d->function, (uint8_t)(cap + 2), (uint16_t)(ctl | 0xC000));
    volatile uint32_t *e = (volatile uint32_t *)(uintptr_t)entry;
    e[0] = addr;
    e[1] = 0;
    e[2] = data;
    e[3] = 0;                                   /* vector control: unmasked */
    pci_write16(d->bus, d->device, d->function, (uint8_t)(cap + 2), (uint16_t)((ctl | 0x8000) & ~0x4000));
    return true;
}

/* MSI: one message, 32- or 64-bit address. */
static bool setup_msi(const struct pci_device *d, uint8_t cap, uint32_t addr, uint32_t data)
{
    uint16_t ctl = pci_read16(d->bus, d->device, d->function, (uint8_t)(cap + 2));
    bool is64 = ctl & 0x80;
    pci_write32(d->bus, d->device, d->function, (uint8_t)(cap + 4), addr);
    if (is64) pci_write32(d->bus, d->device, d->function, (uint8_t)(cap + 8), 0);
    pci_write16(d->bus, d->device, d->function, (uint8_t)(cap + (is64 ? 12 : 8)), (uint16_t)data);
    ctl = (uint16_t)((ctl & ~0x0070) | 0x0001);   /* one message (MME = 0), enable */
    pci_write16(d->bus, d->device, d->function, (uint8_t)(cap + 2), ctl);
    return true;
}

int msi_enable(struct pci_device *dev, msi_handler_t fn, void *arg, const char *name)
{
    if (!apic_enabled() || !fn) return -1;
    uint8_t msix = find_cap(dev, CAP_MSIX), msi = find_cap(dev, CAP_MSI);
    if (!msix && !msi) return -1;
    int i;
    uint64_t f = irq_save();
    for (i = 0; i < VEC_MSI_COUNT && slots[i].fn; i++) {}
    if (i < VEC_MSI_COUNT) { slots[i].fn = fn; slots[i].arg = arg; slots[i].name = name; }
    irq_restore(f);
    if (i == VEC_MSI_COUNT) return -1;
    int vector = VEC_MSI_BASE + i;
    uint32_t addr = MSI_ADDR_BASE | (cpus[0].apic_id << 12);   /* physical destination: the boot CPU */
    uint32_t data = (uint32_t)vector;                          /* fixed delivery, edge */
    bool x = msix && setup_msix(dev, msix, addr, data);
    bool ok = x || (msi && setup_msi(dev, msi, addr, data));
    if (!ok) { slots[i].fn = NULL; return -1; }
    slots[i].msix = x;
    uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND);
    pci_write16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND, (uint16_t)(cmd | PCI_CMD_INTX_DISABLE));
    kprintf("msi: %s at %02x:%02x.%x: vector 0x%x by %s\n", name, dev->bus, dev->device, dev->function, vector, x ? "MSI-X" : "MSI");
    return vector;
}

void msi_disable(struct pci_device *dev, int vector)
{
    if (vector < VEC_MSI_BASE || vector >= VEC_MSI_BASE + VEC_MSI_COUNT) return;
    struct msi_slot *s = &slots[vector - VEC_MSI_BASE];
    uint8_t cap = find_cap(dev, s->msix ? CAP_MSIX : CAP_MSI);
    if (cap) {
        uint16_t ctl = pci_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
        ctl = s->msix ? (uint16_t)(ctl & ~0x8000) : (uint16_t)(ctl & ~0x0001);
        pci_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2), ctl);
    }
    uint64_t f = irq_save();
    s->fn = NULL;
    s->arg = NULL;
    s->msix = false;
    irq_restore(f);
}

bool msi_is_msix(int vector)
{
    return vector >= VEC_MSI_BASE && vector < VEC_MSI_BASE + VEC_MSI_COUNT && slots[vector - VEC_MSI_BASE].msix;
}

void msi_dispatch(int vector)
{
    struct msi_slot *s = &slots[vector - VEC_MSI_BASE];
    s->count++;
    if (s->fn) s->fn(s->arg);
}

size_t msi_proc(char *buf, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < VEC_MSI_COUNT && n < cap; i++)
        if (slots[i].fn)
            n += (size_t)snprintf(buf + n, cap - n, "%3d: %10lu  PCI-MSI  %s\n", VEC_MSI_BASE + i, slots[i].count,
                                  slots[i].name);
    return n < cap ? n : cap;
}
