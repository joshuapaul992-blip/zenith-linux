/* drivers/net/rtl8169.c -- Realtek RTL8169/8110 and RTL8168/8111 gigabit,
 *                          RTL8101/8102/8103/8105/8106/8107/8402 Fast Ethernet
 *
 * The descriptor-based Realtek family that Linux drives with r8169: by far
 * the most common wired Ethernet on PC motherboards and laptops ("Realtek
 * PCIe GBE Family Controller", "Realtek PCIe FE Family Controller").
 * PCI 10ec:8161/8167/8168/8169/8136 and boards built on the 8169 by
 * D-Link, Linksys, US Robotics, Corega and others. The 2.5 and 5 gigabit
 * RTL8125/8126 have a different register layout and are not claimed.
 *
 * The chip is identified from the version bits in TxConfig (the XID, as
 * Linux reads it); the version decides the few settings that differ
 * between generations: the receive configuration word, the transmit FIFO
 * mode, how the PHY is reached (PHYAR before the 8168G, the GPHY OCP
 * window from it on), and the PLL and RXDV gate that a previous driver
 * may have left off. Linux additionally loads per-version PHY and
 * PCIe-PHY tuning tables; this driver does not, and relies on what the
 * firmware and the chip's defaults set up.
 *
 * One receive and one transmit ring of 64 sixteen-byte descriptors with a
 * 2 KiB buffer each, 64-bit bus addresses. Interrupts stay masked: the
 * network thread polls the OWN bit of the next receive descriptor when the
 * system tick sees it clear (net.h).
 *
 * QEMU has no model of these chips, so this driver has only been checked
 * against the documentation and Linux's driver, not run. */
#include <kernel/net.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include "nic.h"

enum {
    IDR0 = 0x00, MAR0 = 0x08, TNPDS = 0x20, CHIPCMD = 0x37, TXPOLL = 0x38, INTRMASK = 0x3C,
    INTRSTATUS = 0x3E, TXCONFIG = 0x40, RXCONFIG = 0x44, CFG9346 = 0x50, CONFIG1 = 0x52, PHYAR = 0x60,
    PHYSTATUS = 0x6C, PMCH = 0x6F, GPHY_OCP = 0xB8, RXMAXSIZE = 0xDA, CPLUSCMD = 0xE0,
    INTRMITIGATE = 0xE2, RDSAR = 0xE4, MAXTXPKT = 0xEC, MISC = 0xF0,
};
#define CMD_RST     0x10
#define CMD_RE      0x08
#define CMD_TE      0x04
#define TXPOLL_NPQ  0x40
#define CFG_UNLOCK  0xC0
#define CFG_LOCK    0x00
#define LINK_STATUS 0x02
#define RX_DMA_BURST    (7u << 8)
#define RX_FIFO_THRESH  (7u << 13)
#define RX_EARLY_OFF    (1u << 11)
#define RX_MULTI_EN     (1u << 14)
#define RX128_INT_EN    (1u << 15)
#define ACCEPT_BCAST    0x08
#define ACCEPT_MCAST    0x04
#define ACCEPT_MYPHYS   0x02
#define TX_DMA_BURST    (7u << 8)
#define TX_IFG          (3u << 24)
#define TXCFG_AUTO_FIFO (1u << 7)
#define CP_PCIMULRW     (1u << 3)
#define CP_RXCHKSUM     (1u << 5)
#define CP_RXVLAN       (1u << 6)
#define RXDV_GATED_EN   (1u << 19)

#define D_OWN   (1u << 31)
#define D_EOR   (1u << 30)
#define D_FS    (1u << 29)
#define D_LS    (1u << 28)
#define RX_RES  (1u << 21)

#define NDESC    64
#define BUF_SIZE 2048

struct desc { uint32_t opts1, opts2; uint64_t addr; } __attribute__((packed));

struct r8169 {
    struct netif nif;
    volatile uint8_t *regs;
    int ver;                            /* Linux's RTL_GIGA_MAC_VER_xx, 0 = unknown */
    volatile struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    uint32_t rx_next, tx_next, tx_clean;
};

static uint8_t r8(struct r8169 *t, uint32_t o) { return *(volatile uint8_t *)(t->regs + o); }
static uint16_t r16(struct r8169 *t, uint32_t o) { return *(volatile uint16_t *)(t->regs + o); }
static uint32_t r32(struct r8169 *t, uint32_t o) { return *(volatile uint32_t *)(t->regs + o); }
static void w8(struct r8169 *t, uint32_t o, uint8_t v) { *(volatile uint8_t *)(t->regs + o) = v; }
static void w16(struct r8169 *t, uint32_t o, uint16_t v) { *(volatile uint16_t *)(t->regs + o) = v; }
static void w32(struct r8169 *t, uint32_t o, uint32_t v) { *(volatile uint32_t *)(t->regs + o) = v; }

static const struct { uint16_t vendor, device; } ids[] = {
    { 0x10ec, 0x8161 }, { 0x10ec, 0x8167 }, { 0x10ec, 0x8168 }, { 0x10ec, 0x8169 }, { 0x10ec, 0x8136 },
    { 0x1186, 0x4300 }, { 0x1186, 0x4302 }, { 0x1259, 0xc107 }, { 0x16ec, 0x0116 },
    { 0x1737, 0x1032 }, { 0x10ff, 0x8168 }, { 0x1734, 0x1159 },
};

/* TxConfig version bits -> Linux's mac_version, first match wins (r8169_main.c). */
static const struct { uint16_t mask, val; uint8_t ver; } xids[] = {
    { 0x7cf, 0x54b, 53 }, { 0x7cf, 0x54a, 52 },                                         /* RTL8117 */
    { 0x7cf, 0x502, 51 }, { 0x7cf, 0x501, 50 }, { 0x7cf, 0x500, 49 },                   /* 8168EP */
    { 0x7cf, 0x541, 46 }, { 0x7cf, 0x540, 45 },                                         /* 8168H */
    { 0x7cf, 0x5c8, 44 }, { 0x7cf, 0x509, 42 }, { 0x7cf, 0x4c1, 41 }, { 0x7cf, 0x4c0, 40 },   /* 8168G */
    { 0x7c8, 0x488, 38 }, { 0x7cf, 0x481, 36 }, { 0x7cf, 0x480, 35 },                   /* 8168F, 8411 */
    { 0x7c8, 0x2c8, 34 }, { 0x7cf, 0x2c1, 32 }, { 0x7c8, 0x2c0, 33 },                   /* 8168E */
    { 0x7cf, 0x281, 25 }, { 0x7c8, 0x280, 26 }, { 0x7c8, 0x288, 27 },                   /* 8168D, 8168DP */
    { 0x7cf, 0x3c9, 23 }, { 0x7cf, 0x3c8, 18 }, { 0x7c8, 0x3c8, 24 }, { 0x7cf, 0x3c0, 19 },   /* 8168C, 8168CP */
    { 0x7cf, 0x3c2, 20 }, { 0x7cf, 0x3c3, 21 }, { 0x7c8, 0x3c0, 22 },
    { 0x7c8, 0x380, 17 }, { 0x7c8, 0x300, 11 },                                         /* 8168B */
    { 0x7c8, 0x448, 39 }, { 0x7c8, 0x440, 37 }, { 0x7cf, 0x409, 29 }, { 0x7c8, 0x408, 30 },   /* 8106E, 8402, 8105E */
    { 0x7cf, 0x349,  8 }, { 0x7cf, 0x249,  8 }, { 0x7cf, 0x348,  7 }, { 0x7cf, 0x248,  7 },   /* 8102E, 8103E */
    { 0x7cf, 0x340, 13 }, { 0x7cf, 0x240, 14 }, { 0x7cf, 0x343, 10 }, { 0x7cf, 0x342, 16 },   /* 8101E */
    { 0x7c8, 0x348,  9 }, { 0x7c8, 0x248,  9 }, { 0x7c8, 0x340, 16 },
    { 0xfc8, 0x388, 13 }, { 0xfc8, 0x308, 13 },
    { 0xfc8, 0x980,  6 }, { 0xfc8, 0x180,  5 }, { 0xfc8, 0x100,  4 }, { 0xfc8, 0x040,  3 }, { 0xfc8, 0x008,  2 },   /* 8169, 8110 */
};

static const char *chip_name(int v)
{
    if (v >= 52) return "RTL8117";
    if (v >= 49) return "RTL8168EP/8111EP";
    if (v >= 45) return "RTL8168H/8111H";
    if (v >= 40) return "RTL8168G/8111G";
    if (v == 39) return "RTL8106E";
    if (v == 38) return "RTL8411";
    if (v == 37) return "RTL8402";
    if (v >= 35) return "RTL8168F/8111F";
    if (v >= 32 && v <= 34) return "RTL8168E/8111E";
    if (v == 29 || v == 30) return "RTL8105E";
    if (v >= 25 && v <= 28) return "RTL8168D/8111D";
    if (v >= 18 && v <= 24) return "RTL8168C/8111C";
    if (v == 11 || v == 17) return "RTL8168B/8111B";
    if (v >= 7 && v <= 9) return "RTL8102E/8103E";
    if (v >= 10 && v <= 16) return "RTL8101E";
    if (v >= 2 && v <= 6) return "RTL8169/8110";
    return "RTL8168 family (unknown version)";
}

/* ---- PHY: MII registers through PHYAR, or the GPHY OCP window (8168G on) ---------- */
static void phy_write(struct r8169 *t, int reg, uint16_t val)
{
    if (t->ver >= 40) {
        uint32_t ocp = 0xa400u + 2u * (uint32_t)reg;            /* standard registers, page 0x0a40 */
        w32(t, GPHY_OCP, 0x80000000u | ocp << 15 | val);
        for (int i = 0; i < 100 && (r32(t, GPHY_OCP) & 0x80000000u); i++) udelay(25);
    } else {
        w32(t, PHYAR, 0x80000000u | (uint32_t)(reg & 0x1f) << 16 | val);
        for (int i = 0; i < 100 && (r32(t, PHYAR) & 0x80000000u); i++) udelay(25);
        udelay(20);
    }
}

/* A driver that shut the chip down (Linux, or the firmware's PXE) may have
 * gated the receive path and powered the PLL and the PHY down. */
static void power_up(struct r8169 *t)
{
    int v = t->ver;
    if ((v >= 25 && v <= 28) || v == 37) w8(t, PMCH, r8(t, PMCH) | 0x80);
    if (v >= 40) {
        w8(t, PMCH, r8(t, PMCH) | 0xc0);
        w32(t, MISC, r32(t, MISC) & ~RXDV_GATED_EN);
    }
    if (v < 40) phy_write(t, 0x1f, 0);                          /* page 0 */
    phy_write(t, 0, 0x1200);                                    /* BMCR: autonegotiate, restart; not powered down */
}

static bool reset(struct r8169 *t)
{
    w8(t, CHIPCMD, CMD_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(r8(t, CHIPCMD) & CMD_RST)) return true;
        udelay(100);
    }
    return false;
}

/* ---- netif operations --------------------------------------------------------- */
static void tx_reclaim(struct r8169 *t)
{
    while (t->tx_clean != t->tx_next && !(t->tx[t->tx_clean].opts1 & D_OWN))
        t->tx_clean = (t->tx_clean + 1) % NDESC;
}

static int r8169_xmit(struct netif *nif, const void *frame, size_t len)
{
    struct r8169 *t = nif->priv;
    if (len > BUF_SIZE) return -1;
    uint32_t next = (t->tx_next + 1) % NDESC;
    if (next == t->tx_clean) tx_reclaim(t);
    if (next == t->tx_clean) return -1;                         /* ring full */
    uint32_t i = t->tx_next;
    uint8_t *b = t->txbuf + (size_t)i * BUF_SIZE;
    memcpy(b, frame, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }   /* some versions send short frames wrong */
    t->tx[i].opts2 = 0;
    nic_fence();
    t->tx[i].opts1 = D_OWN | D_FS | D_LS | (i == NDESC - 1 ? D_EOR : 0) | (uint32_t)len;
    t->tx_next = next;
    nic_fence();
    w8(t, TXPOLL, TXPOLL_NPQ);
    return 0;
}

static void r8169_poll(struct netif *nif)
{
    struct r8169 *t = nif->priv;
    int n = 0;
    uint32_t s;
    while (!((s = t->rx[t->rx_next].opts1) & D_OWN) && n < 64) {
        nic_fence();
        uint32_t i = t->rx_next;
        uint32_t len = s & 0x3fff;
        if ((s & (D_FS | D_LS)) == (D_FS | D_LS) && !(s & RX_RES) && len > 4 + 14)
            net_rx_frame(nif, t->rxbuf + (size_t)i * BUF_SIZE, len - 4);    /* without the CRC */
        else nif->rx_errors++;
        t->rx[i].opts2 = 0;
        nic_fence();
        t->rx[i].opts1 = D_OWN | (i == NDESC - 1 ? D_EOR : 0) | BUF_SIZE;   /* back to the chip */
        t->rx_next = (i + 1) % NDESC;
        n++;
    }
    uint16_t st = r16(t, INTRSTATUS);
    if (st) w16(t, INTRSTATUS, st);                             /* acknowledge (RX FIFO overflow, descriptors unavailable) */
    tx_reclaim(t);
}

static bool r8169_pending(struct netif *nif)
{
    struct r8169 *t = nif->priv;
    return !(t->rx[t->rx_next].opts1 & D_OWN);
}

static bool r8169_link(struct netif *nif)
{
    struct r8169 *t = nif->priv;
    return (r8(t, PHYSTATUS) & LINK_STATUS) != 0;
}

static bool r8169_init(struct pci_device *d)
{
    struct r8169 *t = kzalloc(sizeof *t);
    if (!t) return false;
    pci_enable(d, false, true, true);
    t->regs = nic_mmio(d, 0, NULL);                             /* BAR1 on the 8169, BAR2 on PCIe parts: the first memory BAR */
    if (!t->regs) { kprintf("r8169: %02x:%02x.%u: no register window\n", d->bus, d->device, d->function); kfree(t); return false; }

    uint32_t xid = r32(t, TXCONFIG) >> 20 & 0xfcf;
    if ((xid & 0x7cf) == 0x609 || (xid & 0x7cf) == 0x641 || (xid & 0x7cf) == 0x688 || (xid & 0x7cf) == 0x649 ||
        (xid & 0x7cf) == 0x64a) {
        kprintf("r8169: %02x:%02x.%u: RTL8125/8126 (2.5/5 GbE) is not supported\n", d->bus, d->device, d->function);
        kfree(t);
        return false;
    }
    for (size_t i = 0; i < sizeof xids / sizeof *xids; i++)
        if ((xid & xids[i].mask) == xids[i].val) { t->ver = xids[i].ver; break; }

    w16(t, INTRMASK, 0);
    w16(t, INTRSTATUS, 0xffff);
    if (t->ver >= 40) w32(t, MISC, r32(t, MISC) | RXDV_GATED_EN);   /* quiesce the receive path before the reset */
    if (!reset(t)) { kprintf("r8169: %s does not come out of reset\n", chip_name(t->ver)); kfree(t); return false; }
    power_up(t);

    struct netif *n = &t->nif;
    for (int i = 0; i < 6; i++) n->mac[i] = r8(t, IDR0 + (uint32_t)i);
    if (!(n->mac[0] | n->mac[1] | n->mac[2] | n->mac[3] | n->mac[4] | n->mac[5]) || (n->mac[0] & 1)) {
        uint64_t r = rdtsc();                                   /* no address programmed: a random local one */
        n->mac[0] = 0x02; for (int i = 1; i < 6; i++) n->mac[i] = (uint8_t)(r >> (8 * i));
        w8(t, CFG9346, CFG_UNLOCK);
        w32(t, IDR0 + 4, (uint32_t)n->mac[4] | (uint32_t)n->mac[5] << 8);
        w32(t, IDR0, (uint32_t)n->mac[0] | (uint32_t)n->mac[1] << 8 | (uint32_t)n->mac[2] << 16 | (uint32_t)n->mac[3] << 24);
        w8(t, CFG9346, CFG_LOCK);
    }

    t->rx = nic_dma(NDESC * sizeof(struct desc), false);
    t->tx = nic_dma(NDESC * sizeof(struct desc), false);
    t->rxbuf = nic_dma((size_t)NDESC * BUF_SIZE, false);
    t->txbuf = nic_dma((size_t)NDESC * BUF_SIZE, false);
    if (!t->rx || !t->tx || !t->rxbuf || !t->txbuf) { kprintf("r8169: out of memory\n"); return false; }
    for (uint32_t i = 0; i < NDESC; i++) {
        t->rx[i].addr = nic_bus(t->rxbuf + (size_t)i * BUF_SIZE);
        t->rx[i].opts1 = D_OWN | (i == NDESC - 1 ? D_EOR : 0) | BUF_SIZE;
        t->tx[i].addr = nic_bus(t->txbuf + (size_t)i * BUF_SIZE);
        t->tx[i].opts1 = i == NDESC - 1 ? D_EOR : 0;
    }

    /* The order of rtl_hw_start() in Linux: configuration registers unlocked,
     * C+ mode, ring addresses, then transmitter and receiver on, then their
     * configuration words. */
    w8(t, CFG9346, CFG_UNLOCK);
    uint16_t cp = r16(t, CPLUSCMD) & ~(CP_RXVLAN | CP_RXCHKSUM);
    if (t->ver && t->ver <= 6) cp |= CP_PCIMULRW;
    w16(t, CPLUSCMD, cp);
    w8(t, MAXTXPKT, 0x3f);                                      /* 8169: no early transmit; 8168: largest packet */
    w16(t, INTRMITIGATE, 0);
    w16(t, RXMAXSIZE, BUF_SIZE);
    w32(t, TNPDS + 4, (uint32_t)(nic_bus(t->tx) >> 32));
    w32(t, TNPDS, (uint32_t)nic_bus(t->tx));
    w32(t, RDSAR + 4, (uint32_t)(nic_bus(t->rx) >> 32));
    w32(t, RDSAR, (uint32_t)nic_bus(t->rx));
    w8(t, CFG9346, CFG_LOCK);
    (void)r8(t, CHIPCMD);                                       /* flush the posted writes */
    w8(t, CHIPCMD, CMD_TE | CMD_RE);

    int v = t->ver;
    uint32_t rxcfg;
    if ((v >= 2 && v <= 6) || (v >= 10 && v <= 17)) rxcfg = RX_FIFO_THRESH | RX_DMA_BURST;
    else if ((v >= 18 && v <= 24) || (v >= 34 && v <= 36) || v == 38) rxcfg = RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST;
    else if (v >= 40) rxcfg = RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF;
    else rxcfg = RX128_INT_EN | RX_DMA_BURST;
    w32(t, RXCONFIG, rxcfg);
    w32(t, TXCONFIG, TX_DMA_BURST | TX_IFG | (v >= 34 && v != 39 ? TXCFG_AUTO_FIFO : 0));
    w32(t, MAR0 + 4, 0xffffffff);                               /* every multicast group; the stack filters */
    w32(t, MAR0, 0xffffffff);
    w32(t, RXCONFIG, rxcfg | ACCEPT_BCAST | ACCEPT_MCAST | ACCEPT_MYPHYS);

    n->driver = "r8169";
    n->mtu = ETH_MTU;
    n->xmit = r8169_xmit;
    n->poll = r8169_poll;
    n->pending = r8169_pending;
    n->link_up = r8169_link;
    n->priv = t;
    kprintf("r8169: Realtek %s (XID %03x, version %d) at %02x:%02x.%u\n", chip_name(v), xid, v, d->bus, d->device, d->function);
    netif_register(n);
    return true;
}

void rtl8169_probe(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        for (size_t k = 0; k < sizeof ids / sizeof *ids; k++)
            if (d->vendor_id == ids[k].vendor && d->device_id == ids[k].device) { r8169_init(d); break; }
    }
}
