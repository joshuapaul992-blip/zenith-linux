/* drivers/net/virtio_net.c -- virtio network card (virtio 1.x over PCI)
 *
 * QEMU's and most hypervisors' paravirtual NIC: PCI 1af4:1000 (transitional)
 * or 1af4:1041 (modern). The driver uses the modern interface only: the
 * common, notify and device configuration structures found through the
 * vendor capabilities, two split virtqueues (0 receive, 1 transmit) of up
 * to 128 entries, one 2 KiB buffer per descriptor, and the 12-byte
 * virtio_net_hdr with no offloads negotiated. Interrupts stay off on both
 * queues: the network thread polls the used rings when the system tick
 * sees them move (net.h). */
#include <kernel/net.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include "pci.h"

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_NET_LEGACY   0x1000
#define VIRTIO_NET_MODERN   0x1041

#define CAP_COMMON  1
#define CAP_NOTIFY  2
#define CAP_ISR     3
#define CAP_DEVICE  4

#define ST_ACK          1
#define ST_DRIVER       2
#define ST_DRIVER_OK    4
#define ST_FEATURES_OK  8
#define ST_FAILED       128

#define F_NET_MAC       (1ull << 5)
#define F_NET_STATUS    (1ull << 16)
#define F_VERSION_1     (1ull << 32)

#define DESC_NEXT   1
#define DESC_WRITE  2
#define AVAIL_NO_INTERRUPT 1

#define QMAX        128
#define BUF_SIZE    2048
#define HDR_LEN     12

struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags, next; } __attribute__((packed));
struct vq_avail { uint16_t flags, idx, ring[]; } __attribute__((packed));
struct vq_used_elem { uint32_t id, len; } __attribute__((packed));
struct vq_used { uint16_t flags, idx; struct vq_used_elem ring[]; } __attribute__((packed));

struct vq {
    uint16_t size, index;
    volatile struct vq_desc *desc;
    volatile struct vq_avail *avail;
    volatile struct vq_used *used;
    volatile uint16_t *notify;
    uint16_t avail_idx, last_used;
    uint8_t *bufs;                      /* size * BUF_SIZE */
    uint16_t free[QMAX];                /* transmit: free descriptor ids */
    int      nfree;
};

struct vnet {
    struct netif nif;
    struct pci_device *pci;
    volatile uint8_t *common, *isr, *devcfg, *notify_base;
    uint32_t notify_mult;
    uint64_t features;
    struct vq rx, tx;
};

#define C8(v, o)  (*(volatile uint8_t  *)((v)->common + (o)))
#define C16(v, o) (*(volatile uint16_t *)((v)->common + (o)))
#define C32(v, o) (*(volatile uint32_t *)((v)->common + (o)))
#define C64(v, o) (*(volatile uint64_t *)((v)->common + (o)))
enum { DFSEL = 0, DF = 4, GFSEL = 8, GF = 12, MSIX = 16, NUMQ = 18, STATUS = 20, QSEL = 22, QSIZE = 24,
       QMSIX = 26, QENABLE = 28, QNOFF = 30, QDESC = 32, QDRIVER = 40, QDEVICE = 48 };

static void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static volatile uint8_t *map_cap(struct pci_device *d, uint8_t bar, uint32_t off, uint32_t len)
{
    if (bar > 5) return NULL;
    struct pci_bar *b = &d->bars[bar];
    if (b->kind != PCI_BAR_MEM32 && b->kind != PCI_BAR_MEM64) return NULL;
    if (!b->base || (b->size && (uint64_t)off + len > b->size)) return NULL;
    uint64_t phys = b->base + off;
    if (!vmm_set_uncached(phys, len ? len : 4096)) return NULL;
    return (volatile uint8_t *)(uintptr_t)phys;
}

static bool find_caps(struct vnet *v)
{
    struct pci_device *d = v->pci;
    if (!(pci_read16(d->bus, d->device, d->function, PCI_REG_STATUS) & PCI_STATUS_CAP_LIST)) return false;
    uint8_t p = pci_read8(d->bus, d->device, d->function, PCI_REG_CAP_PTR) & 0xfc;
    for (int guard = 0; p && guard < 48; guard++) {
        uint8_t id = pci_read8(d->bus, d->device, d->function, p);
        uint8_t next = pci_read8(d->bus, d->device, d->function, (uint8_t)(p + 1)) & 0xfc;
        if (id == 0x09) {
            uint8_t type = pci_read8(d->bus, d->device, d->function, (uint8_t)(p + 3));
            uint8_t bar = pci_read8(d->bus, d->device, d->function, (uint8_t)(p + 4));
            uint32_t off = pci_read32(d->bus, d->device, d->function, (uint8_t)(p + 8));
            uint32_t len = pci_read32(d->bus, d->device, d->function, (uint8_t)(p + 12));
            switch (type) {
            case CAP_COMMON: if (!v->common) v->common = map_cap(d, bar, off, len); break;
            case CAP_ISR:    if (!v->isr) v->isr = map_cap(d, bar, off, len); break;
            case CAP_DEVICE: if (!v->devcfg) v->devcfg = map_cap(d, bar, off, len); break;
            case CAP_NOTIFY:
                if (!v->notify_base) {
                    v->notify_base = map_cap(d, bar, off, len);
                    v->notify_mult = pci_read32(d->bus, d->device, d->function, (uint8_t)(p + 16));
                }
                break;
            }
        }
        p = next;
    }
    return v->common && v->notify_base && v->devcfg;
}

static void *dma_pages(size_t bytes)
{
    size_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = frames == 1 ? pmm_alloc() : pmm_alloc_contig(frames);
    if (!phys) return NULL;
    memset((void *)(uintptr_t)phys, 0, frames * PAGE_SIZE);
    return (void *)(uintptr_t)phys;     /* RAM is identity-mapped */
}

static bool vq_setup(struct vnet *v, struct vq *q, uint16_t index)
{
    C16(v, QSEL) = index;
    uint16_t max = C16(v, QSIZE);
    if (!max) return false;
    q->size = max < QMAX ? max : QMAX;
    q->index = index;
    q->desc = dma_pages(16u * q->size);
    q->avail = dma_pages(6u + 2u * q->size);
    q->used = dma_pages(6u + 8u * q->size);
    q->bufs = dma_pages((size_t)q->size * BUF_SIZE);
    if (!q->desc || !q->avail || !q->used || !q->bufs) return false;
    for (uint16_t i = 0; i < q->size; i++) {
        q->desc[i].addr = (uint64_t)(uintptr_t)(q->bufs + (size_t)i * BUF_SIZE);
        q->desc[i].len = BUF_SIZE;
        q->desc[i].flags = 0;
        q->desc[i].next = 0;
    }
    q->avail->flags = AVAIL_NO_INTERRUPT;
    C16(v, QSIZE) = q->size;
    C16(v, QMSIX) = 0xffff;             /* no MSI-X vector */
    C64(v, QDESC) = (uint64_t)(uintptr_t)q->desc;
    C64(v, QDRIVER) = (uint64_t)(uintptr_t)q->avail;
    C64(v, QDEVICE) = (uint64_t)(uintptr_t)q->used;
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)C16(v, QNOFF) * v->notify_mult);
    C16(v, QENABLE) = 1;
    return true;
}

static void vq_kick(struct vq *q)
{
    barrier();
    q->avail->idx = q->avail_idx;
    barrier();
    *q->notify = q->index;
}

/* ---- netif operations --------------------------------------------------------- */
static void reclaim_tx(struct vnet *v)
{
    struct vq *q = &v->tx;
    while (q->last_used != q->used->idx) {
        barrier();
        uint32_t id = q->used->ring[q->last_used % q->size].id;
        q->last_used++;
        if (id < q->size && q->nfree < q->size) q->free[q->nfree++] = (uint16_t)id;
    }
}

static int vnet_xmit(struct netif *nif, const void *frame, size_t len)
{
    struct vnet *v = nif->priv;
    struct vq *q = &v->tx;
    if (len > BUF_SIZE - HDR_LEN) return -1;
    if (!q->nfree) reclaim_tx(v);
    if (!q->nfree) return -1;
    uint16_t id = q->free[--q->nfree];
    uint8_t *b = q->bufs + (size_t)id * BUF_SIZE;
    memset(b, 0, HDR_LEN);
    memcpy(b + HDR_LEN, frame, len);
    q->desc[id].len = (uint32_t)(HDR_LEN + len);
    q->desc[id].flags = 0;
    q->avail->ring[q->avail_idx % q->size] = id;
    q->avail_idx++;
    vq_kick(q);
    return 0;
}

static void vnet_poll(struct netif *nif)
{
    struct vnet *v = nif->priv;
    struct vq *q = &v->rx;
    int n = 0;
    while (q->last_used != q->used->idx && n < 64) {
        barrier();
        struct vq_used_elem e = q->used->ring[q->last_used % q->size];
        q->last_used++;
        if (e.id < q->size && e.len > HDR_LEN)
            net_rx_frame(nif, q->bufs + (size_t)e.id * BUF_SIZE + HDR_LEN, e.len - HDR_LEN);
        if (e.id < q->size) {                           /* give the buffer back */
            q->desc[e.id].len = BUF_SIZE;
            q->desc[e.id].flags = DESC_WRITE;
            q->avail->ring[q->avail_idx % q->size] = (uint16_t)e.id;
            q->avail_idx++;
        }
        n++;
    }
    if (n) vq_kick(q);
    reclaim_tx(v);
}

static bool vnet_pending(struct netif *nif)
{
    struct vnet *v = nif->priv;
    return v->rx.last_used != v->rx.used->idx;
}

static bool vnet_link(struct netif *nif)
{
    struct vnet *v = nif->priv;
    if (!(v->features & F_NET_STATUS)) return true;
    return (*(volatile uint16_t *)(v->devcfg + 6) & 1) != 0;
}

static bool vnet_init(struct pci_device *d)
{
    struct vnet *v = kzalloc(sizeof *v);
    if (!v) return false;
    v->pci = d;
    pci_enable(d, false, true, true);
    if (!find_caps(v)) {
        kprintf("virtio-net: %02x:%02x.%u has no modern (virtio 1.0) interface, skipped\n", d->bus, d->device, d->function);
        kfree(v);
        return false;
    }
    C8(v, STATUS) = 0;                                  /* reset */
    for (int i = 0; i < 100000 && C8(v, STATUS); i++) cpu_relax();
    C8(v, STATUS) = ST_ACK;
    C8(v, STATUS) = ST_ACK | ST_DRIVER;
    C32(v, DFSEL) = 0;
    uint64_t have = C32(v, DF);
    C32(v, DFSEL) = 1;
    have |= (uint64_t)C32(v, DF) << 32;
    if (!(have & F_VERSION_1)) { C8(v, STATUS) = ST_FAILED; kfree(v); return false; }
    v->features = have & (F_VERSION_1 | F_NET_MAC | F_NET_STATUS);
    C32(v, GFSEL) = 0; C32(v, GF) = (uint32_t)v->features;
    C32(v, GFSEL) = 1; C32(v, GF) = (uint32_t)(v->features >> 32);
    C8(v, STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK;
    if (!(C8(v, STATUS) & ST_FEATURES_OK)) { C8(v, STATUS) = ST_FAILED; kfree(v); return false; }
    C16(v, MSIX) = 0xffff;
    if (!vq_setup(v, &v->rx, 0) || !vq_setup(v, &v->tx, 1)) {
        kprintf("virtio-net: queue setup failed\n");
        C8(v, STATUS) = ST_FAILED;
        return false;
    }
    for (uint16_t i = 0; i < v->rx.size; i++) {        /* every receive buffer to the device */
        v->rx.desc[i].flags = DESC_WRITE;
        v->rx.avail->ring[i] = i;
    }
    v->rx.avail_idx = v->rx.size;
    for (uint16_t i = 0; i < v->tx.size; i++) v->tx.free[i] = (uint16_t)(v->tx.size - 1 - i);
    v->tx.nfree = v->tx.size;
    C8(v, STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK;
    vq_kick(&v->rx);

    struct netif *n = &v->nif;
    n->driver = "virtio-net";
    n->mtu = ETH_MTU;
    if (v->features & F_NET_MAC) for (int i = 0; i < 6; i++) n->mac[i] = v->devcfg[i];
    else { uint64_t r = rdtsc(); n->mac[0] = 0x52; n->mac[1] = 0x54; for (int i = 2; i < 6; i++) n->mac[i] = (uint8_t)(r >> (8 * i)); }
    n->xmit = vnet_xmit;
    n->poll = vnet_poll;
    n->pending = vnet_pending;
    n->link_up = vnet_link;
    n->priv = v;
    netif_register(n);
    return true;
}

void virtio_net_probe(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->vendor_id != VIRTIO_VENDOR) continue;
        if (d->device_id != VIRTIO_NET_LEGACY && d->device_id != VIRTIO_NET_MODERN) continue;
        if (d->device_id == VIRTIO_NET_LEGACY && d->subsystem_id != 1) continue;   /* transitional: subsystem 1 = net */
        vnet_init(d);
    }
}
