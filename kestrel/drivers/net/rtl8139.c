/* drivers/net/rtl8139.c -- Realtek RTL8139 family Fast Ethernet (10/100)
 *
 * RTL8139/8139A/B/C/C+/D/8100/8130 and the many boards built on them
 * (PCI 10ec:8139, 10ec:8138, 1113:1211 Accton, 1186:1300 D-Link DFE-538TX,
 * 1186:1340, 13d1:ab06 Abocom, 1259:a117 Corega, 14ea:ab06 Planex,
 * 4033:1360 Addtron, 1743:8139 Peppercon, 021b:8139 HP, 018a:0106 LevelOne,
 * 126c:1211 Nortel, 1500:1360 Delta, 16ec:ab06 US Robotics).
 * QEMU: `-device rtl8139`.
 *
 * The chip receives into one ring buffer (8 KiB + 16, with the WRAP bit so
 * a frame never splits at the end) where each frame is preceded by a
 * 4-byte header (status, length with CRC), and sends from four fixed
 * transmit slots used in turn. Both take 32-bit bus addresses, so the
 * buffers come from below 4 GiB. Registers through the I/O port BAR, or
 * the memory BAR when there is none. Interrupts stay masked: the network
 * thread polls the "receive buffer empty" bit when the system tick sees it
 * clear (net.h). */
#include <kernel/net.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include "nic.h"

enum {
    IDR0 = 0x00, MAR0 = 0x08, TSD0 = 0x10, TSAD0 = 0x20, RBSTART = 0x30, CR = 0x37, CAPR = 0x38,
    CBR = 0x3A, IMR = 0x3C, ISR = 0x3E, TCR = 0x40, RCR = 0x44, MPC = 0x4C, CFG9346 = 0x50,
    CONFIG1 = 0x52, MSR = 0x58, BMCR = 0x62,
};
#define CR_BUFE  0x01
#define CR_TE    0x04
#define CR_RE    0x08
#define CR_RST   0x10
#define RCR_APM  (1u << 1)
#define RCR_AM   (1u << 2)
#define RCR_AB   (1u << 3)
#define RCR_WRAP (1u << 7)
#define RCR_MXDMA_UNLIM (7u << 8)
#define RCR_RXFTH_NONE  (7u << 13)
#define TCR_MXDMA_1K    (6u << 8)
#define TCR_IFG_STD     (3u << 24)
#define TSD_OWN  (1u << 13)
#define TSD_TOK  (1u << 15)
#define TSD_ERTXTH(x) ((uint32_t)(x) << 16)         /* early transmit threshold, 32-byte units */
#define RX_ROK   0x0001
#define MSR_LINKB 0x04                              /* set: link down */

#define RX_RING  8192
#define RX_ALLOC (RX_RING + 16 + 1536)              /* WRAP: the chip writes past the end instead of wrapping */
#define TX_SLOTS 4
#define TX_BUF   2048

struct rtl {
    struct netif nif;
    uint16_t io;                        /* I/O port base, or 0 */
    volatile uint8_t *mm;               /* else the memory window */
    uint8_t *rx;
    uint32_t rx_off;
    uint8_t *tx[TX_SLOTS];
    bool tx_busy[TX_SLOTS];
    int tx_next, tx_clean;
};

static uint8_t r8(struct rtl *r, uint16_t o) { return r->io ? inb(r->io + o) : *(volatile uint8_t *)(r->mm + o); }
static uint16_t r16(struct rtl *r, uint16_t o) { return r->io ? inw(r->io + o) : *(volatile uint16_t *)(r->mm + o); }
static uint32_t r32(struct rtl *r, uint16_t o) { return r->io ? inl(r->io + o) : *(volatile uint32_t *)(r->mm + o); }
static void w8(struct rtl *r, uint16_t o, uint8_t v) { if (r->io) outb(r->io + o, v); else *(volatile uint8_t *)(r->mm + o) = v; }
static void w16(struct rtl *r, uint16_t o, uint16_t v) { if (r->io) outw(r->io + o, v); else *(volatile uint16_t *)(r->mm + o) = v; }
static void w32(struct rtl *r, uint16_t o, uint32_t v) { if (r->io) outl(r->io + o, v); else *(volatile uint32_t *)(r->mm + o) = v; }

static const struct { uint16_t vendor, device; } ids[] = {
    { 0x10ec, 0x8139 }, { 0x10ec, 0x8138 }, { 0x1113, 0x1211 }, { 0x1186, 0x1300 }, { 0x1186, 0x1340 },
    { 0x13d1, 0xab06 }, { 0x1259, 0xa117 }, { 0x1259, 0xa11e }, { 0x14ea, 0xab06 }, { 0x14ea, 0xab07 },
    { 0x11db, 0x1234 }, { 0x1432, 0x9130 }, { 0x02ac, 0x1012 }, { 0x018a, 0x0106 }, { 0x126c, 0x1211 },
    { 0x1743, 0x8139 }, { 0x021b, 0x8139 }, { 0x16ec, 0xab06 }, { 0x4033, 0x1360 }, { 0x1500, 0x1360 },
};

/* Receiver and transmitter on, ring buffer and transmit slots at their start. */
static void start(struct rtl *r)
{
    w16(r, IMR, 0);
    w16(r, ISR, 0xffff);
    for (int i = 0; i < 8; i++) w8(r, (uint16_t)(MAR0 + i), 0xff);   /* every multicast group; the stack filters */
    for (int i = 0; i < TX_SLOTS; i++) r->tx_busy[i] = false;
    r->tx_next = r->tx_clean = 0;
    w8(r, CR, CR_RE | CR_TE);
    w32(r, RCR, RCR_RXFTH_NONE | RCR_MXDMA_UNLIM | RCR_WRAP | RCR_AB | RCR_AM | RCR_APM);
    w32(r, TCR, TCR_IFG_STD | TCR_MXDMA_1K);
    w32(r, RBSTART, (uint32_t)nic_bus(r->rx));
    r->rx_off = 0;
    w16(r, CAPR, (uint16_t)(0 - 16));
    w32(r, MPC, 0);
    w8(r, CR, CR_RE | CR_TE);
}

static bool reset(struct rtl *r)
{
    w8(r, CR, CR_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(r8(r, CR) & CR_RST)) return true;
        udelay(100);
    }
    return false;
}

/* After a receive error (a bad header in the ring), as Linux's 8139too does:
 * reset and start over. Frames in the ring and in the transmit slots are lost;
 * TCP resends them. */
static void restart(struct rtl *r)
{
    reset(r);
    start(r);
}

/* ---- netif operations --------------------------------------------------------- */
static void tx_reclaim(struct rtl *r)
{
    while (r->tx_busy[r->tx_clean]) {
        uint32_t s = r32(r, (uint16_t)(TSD0 + 4 * r->tx_clean));
        if (!(s & TSD_OWN)) break;                      /* still being copied out */
        if (!(s & TSD_TOK)) r->nif.tx_errors++;
        r->tx_busy[r->tx_clean] = false;
        r->tx_clean = (r->tx_clean + 1) % TX_SLOTS;
    }
}

static int rtl_xmit(struct netif *nif, const void *frame, size_t len)
{
    struct rtl *r = nif->priv;
    if (len > 1792) return -1;
    if (r->tx_busy[r->tx_next]) tx_reclaim(r);
    if (r->tx_busy[r->tx_next]) return -1;
    int s = r->tx_next;
    memcpy(r->tx[s], frame, len);
    if (len < 60) { memset(r->tx[s] + len, 0, 60 - len); len = 60; }
    r->tx_busy[s] = true;
    r->tx_next = (s + 1) % TX_SLOTS;
    nic_fence();
    w32(r, (uint16_t)(TSAD0 + 4 * s), (uint32_t)nic_bus(r->tx[s]));
    w32(r, (uint16_t)(TSD0 + 4 * s), (uint32_t)len | TSD_ERTXTH(8));   /* clears OWN: go */
    return 0;
}

static void rtl_poll(struct netif *nif)
{
    struct rtl *r = nif->priv;
    int n = 0;
    while (!(r8(r, CR) & CR_BUFE) && n < 64) {
        nic_fence();
        uint8_t *h = r->rx + r->rx_off;
        uint16_t status = (uint16_t)(h[0] | h[1] << 8), len = (uint16_t)(h[2] | h[3] << 8);
        if (len == 0xfff0) break;                       /* the chip is still writing the header */
        if (!(status & RX_ROK) || len < 64 || len > 1518 + 4) {
            nif->rx_errors++;                           /* the ring is out of step: start the chip over */
            restart(r);
            return;
        }
        net_rx_frame(nif, h + 4, (size_t)len - 4);      /* without the CRC */
        r->rx_off = (r->rx_off + len + 4 + 3) & ~3u;
        if (r->rx_off >= RX_RING) r->rx_off -= RX_RING;
        w16(r, CAPR, (uint16_t)(r->rx_off - 16));
        n++;
    }
    uint16_t isr = r16(r, ISR);
    if (isr) w16(r, ISR, isr);                          /* acknowledge; nothing is delivered as an interrupt */
    tx_reclaim(r);
}

static bool rtl_pending(struct netif *nif)
{
    struct rtl *r = nif->priv;
    return !(r8(r, CR) & CR_BUFE);
}

static bool rtl_link(struct netif *nif)
{
    struct rtl *r = nif->priv;
    return !(r8(r, MSR) & MSR_LINKB);
}

static bool rtl_init(struct pci_device *d)
{
    struct rtl *r = kzalloc(sizeof *r);
    if (!r) return false;
    r->io = nic_ioport(d);
    if (r->io) pci_enable(d, true, false, true);
    else {
        pci_enable(d, false, true, true);
        r->mm = nic_mmio(d, 0, NULL);
        if (!r->mm) { kprintf("rtl8139: %02x:%02x.%u: no registers\n", d->bus, d->device, d->function); kfree(r); return false; }
    }
    w8(r, CONFIG1, 0);                                  /* out of low-power mode */
    if (!reset(r)) { kprintf("rtl8139: %02x:%02x.%u: does not come out of reset\n", d->bus, d->device, d->function); kfree(r); return false; }
    r->rx = nic_dma(RX_ALLOC, true);
    for (int i = 0; i < TX_SLOTS; i++) r->tx[i] = i % 2 ? r->tx[i - 1] + TX_BUF : nic_dma(2 * TX_BUF, true);
    for (int i = 0; i < TX_SLOTS; i++) if (!r->tx[i]) r->rx = NULL;
    if (!r->rx) { kprintf("rtl8139: out of memory below 4 GiB\n"); return false; }

    struct netif *n = &r->nif;
    for (int i = 0; i < 6; i++) n->mac[i] = r8(r, (uint16_t)(IDR0 + i));
    start(r);

    const char *chip = "RTL8139 (unknown revision)";
    static const struct { uint32_t v; const char *name; } revs[] = {    /* TCR hardware version bits, as in Linux's 8139too */
        { 0x60000000, "RTL8139" },   { 0x70000000, "RTL8139 rev K" }, { 0x70800000, "RTL8139A" },
        { 0x72800000, "RTL8139A-G" }, { 0x78000000, "RTL8139B" },     { 0x7C000000, "RTL8130" },
        { 0x74000000, "RTL8139C" },  { 0x78800000, "RTL8100" },       { 0x74400000, "RTL8100B/8139D" },
        { 0x74800000, "RTL8139C+" }, { 0x74C00000, "RTL8101" },
    };
    uint32_t hw = r32(r, TCR) & 0x7cc00000;
    for (size_t i = 0; i < sizeof revs / sizeof *revs; i++) if (revs[i].v == hw) chip = revs[i].name;
    n->driver = "rtl8139";
    n->mtu = ETH_MTU;
    n->xmit = rtl_xmit;
    n->poll = rtl_poll;
    n->pending = rtl_pending;
    n->link_up = rtl_link;
    n->priv = r;
    kprintf("rtl8139: Realtek %s at %02x:%02x.%u (%s 0x%x)\n", chip, d->bus, d->device, d->function,
            r->io ? "ports" : "memory", r->io ? r->io : (unsigned)(uintptr_t)r->mm);
    netif_register(n);
    return true;
}

void rtl8139_probe(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        for (size_t k = 0; k < sizeof ids / sizeof *ids; k++)
            if (d->vendor_id == ids[k].vendor && d->device_id == ids[k].device) {
                rtl_init(d);
                break;
            }
    }
}
