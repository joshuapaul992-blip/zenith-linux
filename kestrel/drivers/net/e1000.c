/* drivers/net/e1000.c -- Intel 8254x and 8257x gigabit Ethernet ("e1000")
 *
 * The PCI and PCIe cards of the PRO/1000 family that use the legacy
 * descriptor format: 82540/82541/82543/82544/82545/82546/82547 (QEMU's
 * `-device e1000` is an 82540EM) and 82571/82572/82573/82574/82583 (QEMU's
 * `-device e1000e` is an 82574L). The ICH/PCH LAN controllers built into
 * Intel chipsets (I217/I218/I219) need flash and PHY handling this driver
 * does not have and are not claimed.
 *
 * One receive and one transmit ring of 128 sixteen-byte descriptors, a
 * 2 KiB buffer per descriptor. The MAC address comes from the receive
 * address registers the card loads from its EEPROM at reset (read through
 * EERD if they are empty). Interrupts stay masked: the network thread
 * polls the descriptor done bits when the system tick sees one set
 * (net.h). The link is brought up with auto speed detection (CTRL.SLU and
 * ASDE); the PHY negotiates on its own. */
#include <kernel/net.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include "nic.h"

enum {
    CTRL = 0x0000, STATUS = 0x0008, EERD = 0x0014, ICR = 0x00C0, IMC = 0x00D8,
    RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818, RDTR = 0x2820,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
    MTA = 0x5200, RAL0 = 0x5400, RAH0 = 0x5404,
};
#define CTRL_ASDE   (1u << 5)
#define CTRL_SLU    (1u << 6)
#define CTRL_ILOS   (1u << 7)
#define CTRL_RST    (1u << 26)
#define CTRL_PHYRST (1u << 31)
#define CTRL_LRST   (1u << 3)
#define STATUS_LU   (1u << 1)
#define RCTL_EN     (1u << 1)
#define RCTL_UPE    (1u << 3)
#define RCTL_MPE    (1u << 4)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT(x)  ((uint32_t)(x) << 4)
#define TCTL_COLD(x) ((uint32_t)(x) << 12)
#define TCTL_RTLC   (1u << 24)
#define RAH_AV      (1u << 31)

#define RXD_DD   0x01
#define RXD_EOP  0x02
#define TXD_EOP  0x01
#define TXD_IFCS 0x02
#define TXD_RS   0x08
#define TXD_DD   0x01

#define NDESC    128
#define BUF_SIZE 2048

struct rxd { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct txd { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

struct e1k {
    struct netif nif;
    volatile uint8_t *regs;
    volatile struct rxd *rx;
    volatile struct txd *tx;
    uint8_t *rxbuf, *txbuf;
    uint32_t rx_next;                   /* next receive descriptor to look at */
    uint32_t tx_tail, tx_clean;         /* next free, oldest in flight */
    bool eerd_new;                      /* 82541/82547/8257x: EERD done = bit 1, address << 2 */
};

static uint32_t rd(struct e1k *e, uint32_t r) { return *(volatile uint32_t *)(e->regs + r); }
static void wr(struct e1k *e, uint32_t r, uint32_t v) { *(volatile uint32_t *)(e->regs + r) = v; }

static const struct { uint16_t id; bool eerd_new; const char *name; } models[] = {
    { 0x1000, false, "82542" },   { 0x1001, false, "82543GC" }, { 0x1004, false, "82543GC" },
    { 0x1008, false, "82544EI" }, { 0x1009, false, "82544EI" }, { 0x100C, false, "82544GC" },
    { 0x100D, false, "82544GC" }, { 0x100E, false, "82540EM" }, { 0x100F, false, "82545EM" },
    { 0x1010, false, "82546EB" }, { 0x1011, false, "82545EM" }, { 0x1012, false, "82546EB" },
    { 0x1013, true,  "82541EI" }, { 0x1015, false, "82540EM" }, { 0x1016, false, "82540EP" },
    { 0x1017, false, "82540EP" }, { 0x1018, true,  "82541EI" }, { 0x1019, true,  "82547EI" },
    { 0x101A, true,  "82547EI" }, { 0x101D, false, "82546EB" }, { 0x101E, false, "82540EP" },
    { 0x1026, false, "82545GM" }, { 0x1027, false, "82545GM" }, { 0x1028, false, "82545GM" },
    { 0x105E, true,  "82571EB" }, { 0x105F, true,  "82571EB" }, { 0x1060, true,  "82571EB" },
    { 0x1075, true,  "82547GI" }, { 0x1076, true,  "82541GI" }, { 0x1077, true,  "82541GI" },
    { 0x1078, true,  "82541ER" }, { 0x1079, false, "82546GB" }, { 0x107A, false, "82546GB" },
    { 0x107B, false, "82546GB" }, { 0x107C, true,  "82541PI" }, { 0x107D, true,  "82572EI" },
    { 0x107E, true,  "82572EI" }, { 0x107F, true,  "82572EI" }, { 0x108B, true,  "82573V" },
    { 0x108C, true,  "82573E" },  { 0x109A, true,  "82573L" },  { 0x10A4, true,  "82571EB" },
    { 0x10B5, false, "82546GB" }, { 0x10B9, true,  "82572EI" }, { 0x10BC, true,  "82571EB" },
    { 0x10D3, true,  "82574L" },  { 0x10F6, true,  "82574LA" }, { 0x150C, true,  "82583V" },
};

static int eeprom_read(struct e1k *e, uint8_t addr)
{
    uint32_t done = e->eerd_new ? 1u << 1 : 1u << 4;
    wr(e, EERD, 1u | (uint32_t)addr << (e->eerd_new ? 2 : 8));
    for (int i = 0; i < 10000; i++) {
        uint32_t v = rd(e, EERD);
        if (v & done) return (int)(v >> 16);
        udelay(5);
    }
    return -1;
}

static bool read_mac(struct e1k *e, uint8_t mac[6])
{
    uint32_t lo = rd(e, RAL0), hi = rd(e, RAH0);
    if ((hi & RAH_AV) && (lo || (hi & 0xffff))) {
        for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (8 * i));
        mac[4] = (uint8_t)hi; mac[5] = (uint8_t)(hi >> 8);
        return true;
    }
    for (int i = 0; i < 3; i++) {
        int w = eeprom_read(e, (uint8_t)i);
        if (w < 0) return false;
        mac[2 * i] = (uint8_t)w; mac[2 * i + 1] = (uint8_t)(w >> 8);
    }
    return true;
}

/* ---- netif operations --------------------------------------------------------- */
static void tx_reclaim(struct e1k *e)
{
    while (e->tx_clean != e->tx_tail && (e->tx[e->tx_clean].status & TXD_DD))
        e->tx_clean = (e->tx_clean + 1) % NDESC;
}

static int e1k_xmit(struct netif *nif, const void *frame, size_t len)
{
    struct e1k *e = nif->priv;
    if (len > BUF_SIZE) return -1;
    uint32_t next = (e->tx_tail + 1) % NDESC;
    if (next == e->tx_clean) tx_reclaim(e);
    if (next == e->tx_clean) return -1;                 /* ring full */
    volatile struct txd *d = &e->tx[e->tx_tail];
    memcpy(e->txbuf + (size_t)e->tx_tail * BUF_SIZE, frame, len);
    d->len = (uint16_t)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    e->tx_tail = next;
    nic_fence();
    wr(e, TDT, e->tx_tail);
    return 0;
}

static void e1k_poll(struct netif *nif)
{
    struct e1k *e = nif->priv;
    int n = 0;
    while ((e->rx[e->rx_next].status & RXD_DD) && n < 64) {
        nic_fence();
        volatile struct rxd *d = &e->rx[e->rx_next];
        if ((d->status & RXD_EOP) && !d->errors && d->len >= 14)
            net_rx_frame(nif, e->rxbuf + (size_t)e->rx_next * BUF_SIZE, d->len);
        else nif->rx_errors++;
        d->status = 0;
        uint32_t done = e->rx_next;
        e->rx_next = (e->rx_next + 1) % NDESC;
        nic_fence();
        wr(e, RDT, done);                               /* hand the descriptor back */
        n++;
    }
    tx_reclaim(e);
}

static bool e1k_pending(struct netif *nif)
{
    struct e1k *e = nif->priv;
    return (e->rx[e->rx_next].status & RXD_DD) != 0;
}

static bool e1k_link(struct netif *nif)
{
    struct e1k *e = nif->priv;
    return (rd(e, STATUS) & STATUS_LU) != 0;
}

static bool e1k_init(struct pci_device *d, bool eerd_new, const char *model)
{
    struct e1k *e = kzalloc(sizeof *e);
    if (!e) return false;
    e->eerd_new = eerd_new;
    pci_enable(d, false, true, true);
    e->regs = nic_mmio(d, 0, NULL);
    if (!e->regs) { kprintf("e1000: %02x:%02x.%u: no register window\n", d->bus, d->device, d->function); kfree(e); return false; }

    wr(e, IMC, 0xffffffff);                             /* quiet, then reset */
    wr(e, CTRL, rd(e, CTRL) | CTRL_RST);
    mdelay(2);
    for (int i = 0; i < 1000 && (rd(e, CTRL) & CTRL_RST); i++) udelay(100);
    mdelay(10);                                         /* the EEPROM reload after reset */
    wr(e, IMC, 0xffffffff);
    (void)rd(e, ICR);
    wr(e, CTRL, (rd(e, CTRL) | CTRL_SLU | CTRL_ASDE) & ~(CTRL_LRST | CTRL_PHYRST | CTRL_ILOS));

    struct netif *n = &e->nif;
    if (!read_mac(e, n->mac)) { kprintf("e1000: %s: cannot read the MAC address\n", model); kfree(e); return false; }
    wr(e, RAL0, (uint32_t)n->mac[0] | (uint32_t)n->mac[1] << 8 | (uint32_t)n->mac[2] << 16 | (uint32_t)n->mac[3] << 24);
    wr(e, RAH0, (uint32_t)n->mac[4] | (uint32_t)n->mac[5] << 8 | RAH_AV);
    for (int i = 0; i < 128; i++) wr(e, MTA + 4 * (uint32_t)i, 0);

    e->rx = nic_dma(NDESC * sizeof(struct rxd), false);
    e->tx = nic_dma(NDESC * sizeof(struct txd), false);
    e->rxbuf = nic_dma((size_t)NDESC * BUF_SIZE, false);
    e->txbuf = nic_dma((size_t)NDESC * BUF_SIZE, false);
    if (!e->rx || !e->tx || !e->rxbuf || !e->txbuf) { kprintf("e1000: out of memory\n"); return false; }
    for (int i = 0; i < NDESC; i++) {
        e->rx[i].addr = nic_bus(e->rxbuf + (size_t)i * BUF_SIZE);
        e->tx[i].addr = nic_bus(e->txbuf + (size_t)i * BUF_SIZE);
        e->tx[i].status = TXD_DD;
    }
    wr(e, RDBAL, (uint32_t)nic_bus(e->rx));
    wr(e, RDBAH, (uint32_t)(nic_bus(e->rx) >> 32));
    wr(e, RDLEN, NDESC * sizeof(struct rxd));
    wr(e, RDH, 0);
    wr(e, RDT, NDESC - 1);                              /* one slot stays empty: head == tail means none free */
    wr(e, RDTR, 0);
    wr(e, RCTL, RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SECRC);    /* 2 KiB buffers (BSIZE 0) */

    wr(e, TDBAL, (uint32_t)nic_bus(e->tx));
    wr(e, TDBAH, (uint32_t)(nic_bus(e->tx) >> 32));
    wr(e, TDLEN, NDESC * sizeof(struct txd));
    wr(e, TDH, 0);
    wr(e, TDT, 0);
    wr(e, TCTL, TCTL_EN | TCTL_PSP | TCTL_CT(0x0F) | TCTL_COLD(0x3F) | TCTL_RTLC);
    wr(e, TIPG, 10 | 8u << 10 | 6u << 20);              /* IEEE 802.3 copper gaps */

    n->driver = "e1000";
    n->mtu = ETH_MTU;
    n->xmit = e1k_xmit;
    n->poll = e1k_poll;
    n->pending = e1k_pending;
    n->link_up = e1k_link;
    n->priv = e;
    kprintf("e1000: Intel %s at %02x:%02x.%u\n", model, d->bus, d->device, d->function);
    netif_register(n);
    return true;
}

void e1000_probe(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->vendor_id != 0x8086) continue;
        for (size_t m = 0; m < sizeof models / sizeof *models; m++)
            if (models[m].id == d->device_id) { e1k_init(d, models[m].eerd_new, models[m].name); break; }
    }
}
