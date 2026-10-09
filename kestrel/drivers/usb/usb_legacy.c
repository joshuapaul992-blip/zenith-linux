/* =============================================================================
 *  usb_legacy.c -- firmware -> OS handoff for EHCI, UHCI, OHCI, Intel xHCI
 *  References: EHCI 1.0 section 5.1 and 2.1.7-2.1.8; UHCI design guide
 *  (LEGSUP); OHCI 1.0a section 5.1.1.3.3; Intel 7/8/9-series PCH datasheets
 *  (XUSB2PR, USB3_PSSEN and their *M mask registers).
 * ============================================================================= */
#include "usb_legacy.h"
#include "pci.h"

#define REG32(base, off) (*(volatile uint32_t *)((uint8_t *)(base) + (off)))

static const struct usb_legacy_platform *P;

static void out_c(char c) { if (P && P->log_putc) P->log_putc(c); }
static void out_s(const char *s) { while (*s) out_c(*s++); }
static void out_hex(uint64_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) out_c(hex[(v >> (4 * i)) & 0xF]);
}
static void out_bdf(const struct pci_device *d)
{
    out_hex(d->bus, 2); out_c(':'); out_hex(d->device, 2); out_c('.'); out_hex(d->function, 1);
}
static void delay_ms(uint32_t ms) { while (ms--) P->delay_us(1000); }

static void *mmio(const struct pci_device *d, int bar, uint32_t size)
{
    const struct pci_bar *b = &d->bars[bar];
    if ((b->kind != PCI_BAR_MEM32 && b->kind != PCI_BAR_MEM64) || !b->base) return NULL;
    return P->map_mmio ? P->map_mmio(b->base, size) : (void *)(uintptr_t)b->base;
}

/* ---- EHCI ------------------------------------------------------------------ */
#define EHCI_LEGSUP_CAPID       0x01
#define EHCI_LEGSUP_BIOS_OWNED  (1u << 16)
#define EHCI_LEGSUP_OS_OWNED    (1u << 24)
#define EHCI_USBCMD_RUN         (1u << 0)
#define EHCI_USBSTS_HALTED      (1u << 12)

static void ehci_handoff(struct pci_device *d, struct usb_legacy_report *rep)
{
    rep->ehci++;
    pci_enable(d, false, true, false);
    volatile uint8_t *base = mmio(d, 0, 0x100);
    if (!base) { out_s("usb-legacy: EHCI "); out_bdf(d); out_s(" has no MMIO BAR0\n"); return; }

    uint32_t caplen = REG32(base, 0) & 0xFF;
    uint32_t hccparams = REG32(base, 0x08);
    uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFF);
    out_s("usb-legacy: EHCI "); out_bdf(d); out_s(" EECP=0x"); out_hex(eecp, 2);

    /* walk the extended capability list in PCI configuration space */
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        uint32_t cap = pci_read32(d->bus, d->device, d->function, eecp);
        if ((cap & 0xFF) == EHCI_LEGSUP_CAPID) {
            if (cap & EHCI_LEGSUP_BIOS_OWNED) {
                rep->ehci_bios_owned++;
                /* request ownership; byte 3 holds the OS semaphore */
                pci_write32(d->bus, d->device, d->function, eecp, cap | EHCI_LEGSUP_OS_OWNED);
                uint32_t waited = 0;
                while ((pci_read32(d->bus, d->device, d->function, eecp) & EHCI_LEGSUP_BIOS_OWNED) && waited < 1000) {
                    delay_ms(10); waited += 10;
                }
                if (pci_read32(d->bus, d->device, d->function, eecp) & EHCI_LEGSUP_BIOS_OWNED) {
                    rep->ehci_forced++;
                    out_s(" BIOS did not release in 1 s, forcing");
                    pci_write32(d->bus, d->device, d->function, eecp,
                                (cap & ~EHCI_LEGSUP_BIOS_OWNED) | EHCI_LEGSUP_OS_OWNED);
                } else {
                    out_s(" BIOS released ownership");
                }
            } else {
                pci_write32(d->bus, d->device, d->function, eecp, cap | EHCI_LEGSUP_OS_OWNED);
                out_s(" OS ownership set (BIOS did not own it)");
            }
            /* USBLEGCTLSTS: disable every SMI enable, acknowledge pending ones */
            pci_write32(d->bus, d->device, d->function, (uint8_t)(eecp + 4), 0xE0000000u);
        }
        eecp = (uint8_t)((cap >> 8) & 0xFF);
    }

    /* Stop the controller and give the ports back to the companions until a
     * real EHCI driver exists (CONFIGFLAG = 0 routes all ports away). */
    volatile uint8_t *op = base + caplen;
    REG32(op, 0x08) = 0;                                    /* USBINTR: all off */
    if (REG32(op, 0x00) & EHCI_USBCMD_RUN) {
        REG32(op, 0x00) = REG32(op, 0x00) & ~EHCI_USBCMD_RUN;
        for (int i = 0; i < 20 && !(REG32(op, 0x04) & EHCI_USBSTS_HALTED); i++) delay_ms(1);
    }
    REG32(op, 0x40) = 0;                                    /* CONFIGFLAG */
    out_s(REG32(op, 0x04) & EHCI_USBSTS_HALTED ? ", halted, ports routed to companions\n"
                                               : ", WARNING: did not halt\n");
}

/* ---- UHCI ------------------------------------------------------------------ */
static void uhci_handoff(struct pci_device *d, struct usb_legacy_report *rep)
{
    rep->uhci++;
    uint16_t legsup = pci_read16(d->bus, d->device, d->function, 0xC0);
    /* 0x8F00: clear the RW1C status bits and every trap/SMI enable */
    pci_write16(d->bus, d->device, d->function, 0xC0, 0x8F00);
    out_s("usb-legacy: UHCI "); out_bdf(d); out_s(" LEGSUP 0x"); out_hex(legsup, 4);
    out_s(" -> 0x8f00 (keyboard emulation and SMIs off)\n");
}

/* ---- OHCI ------------------------------------------------------------------ */
#define OHCI_HCCONTROL      0x04
#define OHCI_HCCOMMANDSTAT  0x08
#define OHCI_HCINTDISABLE   0x14
#define OHCI_CTRL_IR        (1u << 8)
#define OHCI_CMD_OCR        (1u << 3)

static void ohci_handoff(struct pci_device *d, struct usb_legacy_report *rep)
{
    rep->ohci++;
    pci_enable(d, false, true, false);
    volatile uint8_t *base = mmio(d, 0, 0x100);
    if (!base) return;
    out_s("usb-legacy: OHCI "); out_bdf(d);
    if (REG32(base, OHCI_HCCONTROL) & OHCI_CTRL_IR) {
        REG32(base, OHCI_HCCOMMANDSTAT) = OHCI_CMD_OCR;     /* ownership change request */
        uint32_t waited = 0;
        while ((REG32(base, OHCI_HCCONTROL) & OHCI_CTRL_IR) && waited < 1000) { delay_ms(10); waited += 10; }
        if (REG32(base, OHCI_HCCONTROL) & OHCI_CTRL_IR) {
            rep->ohci_forced++;
            REG32(base, OHCI_HCCONTROL) = REG32(base, OHCI_HCCONTROL) & ~OHCI_CTRL_IR;
            out_s(" SMM did not release in 1 s, forcing");
        } else {
            out_s(" SMM released ownership");
        }
    } else {
        out_s(" not owned by SMM");
    }
    REG32(base, OHCI_HCINTDISABLE) = 0x80000000u;           /* master interrupt enable off */
    out_c('\n');
}

/* ---- Intel PCH xHCI port switchover ---------------------------------------- */
#define INTEL_XUSB2PR       0xD0
#define INTEL_XUSB2PRM      0xD4
#define INTEL_USB3_PSSEN    0xD8
#define INTEL_USB3PRM       0xDC

static bool intel_switchable(uint16_t did)
{
    static const uint16_t ids[] = {
        0x1E31,             /* 7 series  (Panther Point)     */
        0x8C31, 0x9C31,     /* 8 series  (Lynx Point, -LP)   */
        0x8CB1, 0x9CB1,     /* 9 series  (Wildcat Point, -LP) */
    };
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) if (ids[i] == did) return true;
    return false;
}

static void intel_xhci_switch(struct pci_device *d, struct usb_legacy_report *rep)
{
    /* The *M registers say which ports the BIOS allows to be switched. */
    uint32_t ss_mask = pci_read32(d->bus, d->device, d->function, INTEL_USB3PRM);
    pci_write32(d->bus, d->device, d->function, INTEL_USB3_PSSEN, ss_mask);
    uint32_t hs_mask = pci_read32(d->bus, d->device, d->function, INTEL_XUSB2PRM);
    pci_write32(d->bus, d->device, d->function, INTEL_XUSB2PR, hs_mask);
    rep->intel_routed++;
    rep->usb3_pssen = pci_read32(d->bus, d->device, d->function, INTEL_USB3_PSSEN);
    rep->xusb2pr = pci_read32(d->bus, d->device, d->function, INTEL_XUSB2PR);
    out_s("usb-legacy: Intel xHCI "); out_bdf(d); out_s(" port switchover: USB3_PSSEN=0x");
    out_hex(rep->usb3_pssen, 8); out_s(" XUSB2PR=0x"); out_hex(rep->xusb2pr, 8); out_c('\n');
}

void usb_legacy_handoff_all(const struct usb_legacy_platform *plat, struct usb_legacy_report *out)
{
    struct usb_legacy_report rep = { 0 };
    P = plat;
    /* EHCI first: Intel switchover must not race an EHCI still run by SMM. */
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < pci_device_count(); i++) {
            struct pci_device *d = pci_device_at(i);
            if (d->class_code != PCI_CLASS_SERIAL_BUS || d->subclass != PCI_SUBCLASS_USB) continue;
            if (pass == 0) {
                if (d->prog_if == 0x20) ehci_handoff(d, &rep);
                else if (d->prog_if == 0x00) uhci_handoff(d, &rep);
                else if (d->prog_if == 0x10) ohci_handoff(d, &rep);
            } else if (d->prog_if == PCI_PROGIF_XHCI && d->vendor_id == 0x8086 && intel_switchable(d->device_id)) {
                intel_xhci_switch(d, &rep);
            }
        }
    if (!rep.ehci && !rep.uhci && !rep.ohci && !rep.intel_routed)
        out_s("usb-legacy: no EHCI/UHCI/OHCI companions, nothing to hand off\n");
    if (out) *out = rep;
}
