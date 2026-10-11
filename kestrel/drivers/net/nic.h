/* drivers/net/nic.h -- small helpers shared by the Ethernet card drivers
 *
 * RAM is identity-mapped, so a buffer's address is also its bus address;
 * register windows are mapped uncached in place. */
#ifndef KESTREL_NIC_H
#define KESTREL_NIC_H

#include <kernel/mm.h>
#include <kernel/string.h>
#include "pci.h"

/* Zeroed, physically contiguous memory for rings and buffers. */
static inline void *nic_dma(size_t bytes, bool below_4g)
{
    size_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = below_4g ? pmm_alloc_dma32(frames) : pmm_alloc_contig(frames);
    if (!phys) return NULL;
    memset((void *)(uintptr_t)phys, 0, frames * PAGE_SIZE);
    return (void *)(uintptr_t)phys;
}

static inline uint64_t nic_bus(const volatile void *p) { return (uint64_t)(uintptr_t)p; }

/* The first memory BAR at or after `from`, mapped uncached; NULL if none. */
static inline volatile uint8_t *nic_mmio(struct pci_device *d, int from, int *which)
{
    for (int i = from; i < 6; i++) {
        struct pci_bar *b = &d->bars[i];
        if ((b->kind != PCI_BAR_MEM32 && b->kind != PCI_BAR_MEM64) || !b->base) continue;
        uint64_t len = b->size ? b->size : 4096;
        if (len > 0x20000) len = 0x20000;                   /* the registers we use are all in the first 128 KiB */
        if (!vmm_set_uncached(b->base, len)) return NULL;
        if (which) *which = i;
        return (volatile uint8_t *)(uintptr_t)b->base;
    }
    return NULL;
}

/* The first I/O port BAR; 0 if none. */
static inline uint16_t nic_ioport(struct pci_device *d)
{
    for (int i = 0; i < 6; i++)
        if (d->bars[i].kind == PCI_BAR_IO && d->bars[i].base) return (uint16_t)d->bars[i].base;
    return 0;
}

static inline void nic_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#endif
