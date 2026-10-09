/* =============================================================================
 *  pci.c -- standalone PCI / PCIe bus enumerator (Configuration Mechanism #1)
 *
 *  Freestanding C11 (GNU extensions for inline asm and attributes). No libc,
 *  no allocator: devices live in a fixed pool, matches in fixed per-category
 *  reference arrays, so the module can run before any memory manager exists.
 *
 *  Address register (0xCF8) layout for Mechanism #1:
 *      31      enable
 *      30:24   reserved (0)
 *      23:16   bus      (0-255)
 *      15:11   device   (0-31)
 *      10:8    function (0-7)
 *       7:2    dword-aligned register offset
 *       1:0    0
 *  The selected dword is then read or written through 0xCFC.
 * ============================================================================= */
#include "pci.h"

/* ============================================================================
 *  Port I/O and interrupt masking
 * ============================================================================ */
#ifdef PCI_PORT_IO_HOOKS
/* Test builds supply these (e.g. a simulated configuration space). */
extern void     pci_hook_outl(uint16_t port, uint32_t v);
extern uint32_t pci_hook_inl(uint16_t port);
extern void     pci_hook_outb(uint16_t port, uint8_t v);
extern uint8_t  pci_hook_inb(uint16_t port);
#define port_outl pci_hook_outl
#define port_inl  pci_hook_inl
#define port_outb pci_hook_outb
#define port_inb  pci_hook_inb
typedef int irqstate_t;
static inline irqstate_t irq_off(void) { return 0; }
static inline void irq_restore_state(irqstate_t s) { (void)s; }
#else
static inline void port_outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint32_t port_inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void port_outb(uint16_t port, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t port_inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* The 0xCF8/0xCFC pair is a two-step protocol: an interrupt handler that
 * also touches config space between the two steps would corrupt the
 * access, so each access runs with interrupts disabled. (SMP kernels must
 * additionally hold a spinlock around these sections.) */
typedef uint64_t irqstate_t;
static inline irqstate_t irq_off(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore_state(irqstate_t f)
{
    if (f & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}
#endif

/* ============================================================================
 *  Fallbacks for compiler-generated memset/memcpy calls (weak: a kernel's
 *  own definitions take precedence at link time).
 * ============================================================================ */
#ifndef PCI_NO_MEM_FALLBACK
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
#endif

/* ============================================================================
 *  State
 * ============================================================================ */
static struct pci_device      g_pool[PCI_MAX_TRACKED];
static size_t                 g_count;
static struct pci_device     *g_matches[PCI_MATCH_CATEGORIES][PCI_MAX_MATCHES];
static size_t                 g_match_count[PCI_MATCH_CATEGORIES];
static uint8_t                g_bus_visited[PCI_MAX_BUSES / 8];
static struct pci_scan_stats  g_stats;
static unsigned               g_flags;

#define MAX_BRIDGE_DEPTH 32     /* guards against firmware bus-number loops */

/* ============================================================================
 *  Configuration space access
 * ============================================================================ */
static inline uint32_t cfg_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return PCI_CONFIG_ENABLE | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    irqstate_t s = irq_off();
    port_outl(PCI_CONFIG_ADDRESS, cfg_address(bus, dev, func, off));
    uint32_t v = port_inl(PCI_CONFIG_DATA);
    irq_restore_state(s);
    g_stats.config_reads++;
    return v;
}

/* Narrow reads use a full dword read and shift: every chipset supports
 * 32-bit accesses to 0xCFC, while byte/word accesses at 0xCFD-0xCFF are
 * occasionally mishandled by emulators and old bridges. */
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint8_t)(pci_read32(bus, dev, func, off) >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v)
{
    irqstate_t s = irq_off();
    port_outl(PCI_CONFIG_ADDRESS, cfg_address(bus, dev, func, off));
    port_outl(PCI_CONFIG_DATA, v);
    irq_restore_state(s);
}

/* Read-modify-write of the containing dword. Do not use this on registers
 * with write-1-to-clear bits in the other half (e.g. Status next to
 * Command): pci_enable() handles that case explicitly. */
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t v)
{
    unsigned shift = (off & 2) * 8;
    uint32_t d = pci_read32(bus, dev, func, off);
    d = (d & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, dev, func, off, d);
}

/* Write the Command register without echoing back Status bits: Status is
 * RW1C, so writing the value just read would clear pending error flags. */
static void write_command(uint8_t bus, uint8_t dev, uint8_t func, uint16_t cmd)
{
    pci_write32(bus, dev, func, PCI_REG_COMMAND, cmd);   /* upper half (Status) = 0: no effect */
}

bool pci_mech1_present(void)
{
    irqstate_t s = irq_off();
    uint32_t saved = port_inl(PCI_CONFIG_ADDRESS);
    port_outl(PCI_CONFIG_ADDRESS, PCI_CONFIG_ENABLE);
    bool ok = port_inl(PCI_CONFIG_ADDRESS) == PCI_CONFIG_ENABLE;
    port_outl(PCI_CONFIG_ADDRESS, saved);
    irq_restore_state(s);
    return ok;
}

/* ============================================================================
 *  Debug output (polled 16550 UART on COM1 unless a sink is installed)
 * ============================================================================ */
#define COM1 0x3F8
static void (*g_sink)(char);

void pci_debug_init(void)
{
    port_outb(COM1 + 1, 0x00);      /* no UART interrupts           */
    port_outb(COM1 + 3, 0x80);      /* DLAB                         */
    port_outb(COM1 + 0, 0x01);      /* divisor 1 = 115200 baud      */
    port_outb(COM1 + 1, 0x00);
    port_outb(COM1 + 3, 0x03);      /* 8N1                          */
    port_outb(COM1 + 2, 0xC7);      /* FIFO on, cleared             */
    port_outb(COM1 + 4, 0x0B);      /* DTR, RTS, OUT2               */
}

void pci_set_log_sink(void (*putc_fn)(char c)) { g_sink = putc_fn; }

static void com1_putc(char c)
{
    for (int spin = 0; spin < 100000 && !(port_inb(COM1 + 5) & 0x20); spin++)
        __asm__ volatile("pause");
    port_outb(COM1, (uint8_t)c);
}

static void out_c(char c)
{
    if (g_sink) { g_sink(c); return; }
    if (c == '\n') com1_putc('\r');
    com1_putc(c);
}

static void out_s(const char *s) { while (*s) out_c(*s++); }

static void out_hex(uint64_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) out_c(hex[(v >> (i * 4)) & 0xF]);
}

static void out_hex_min(uint64_t v)        /* without leading zeros */
{
    int digits = 1;
    for (uint64_t t = v >> 4; t; t >>= 4) digits++;
    out_hex(v, digits);
}

static void out_dec(uint64_t v)
{
    char buf[21]; int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_c(buf[--n]);
}

static void out_size(uint64_t bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    while (bytes >= 1024 && (bytes & 1023) == 0 && u < 4) { bytes >>= 10; u++; }
    out_dec(bytes); out_c(' '); out_s(units[u]);
}

/* ============================================================================
 *  Names
 * ============================================================================ */
const char *pci_class_name(uint8_t c, uint8_t s, uint8_t p)
{
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (s) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return p == 0x01 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return p == 0x02 ? "NVM controller (NVMe)" : "Non-volatile memory controller";
        default:   return "Mass storage controller";
        }
    case 0x02: return s == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03:
        switch (s) {
        case 0x00: return p == 0x01 ? "8514-compatible display" : "VGA-compatible display";
        case 0x02: return "3D controller";
        default:   return "Display controller";
        }
    case 0x04: return s == 0x03 ? "Audio device (HD Audio)" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x07: return "CardBus bridge";
        case 0x09: return "PCI-to-PCI bridge (semi-transparent)";
        default:   return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (s) {
        case 0x03:
            switch (p) {
            case 0x00: return "USB controller (UHCI)";
            case 0x10: return "USB controller (OHCI)";
            case 0x20: return "USB controller (EHCI)";
            case 0x30: return "USB controller (xHCI)";
            case 0x40: return "USB4 host interface";
            default:   return "USB controller";
            }
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    case 0xFF: return "Vendor-specific";
    default:   return "Other device";
    }
}

static int match_index(enum pci_match m)
{
    switch (m) {
    case PCI_MATCH_AHCI:           return 0;
    case PCI_MATCH_NVME:           return 1;
    case PCI_MATCH_NVIDIA_DISPLAY: return 2;
    case PCI_MATCH_XHCI:           return 3;
    }
    return -1;
}

const char *pci_match_name(enum pci_match m)
{
    switch (m) {
    case PCI_MATCH_AHCI:           return "AHCI";
    case PCI_MATCH_NVME:           return "NVMe";
    case PCI_MATCH_NVIDIA_DISPLAY: return "NVIDIA display";
    case PCI_MATCH_XHCI:           return "xHCI";
    }
    return "?";
}

/* ============================================================================
 *  Classification and filtering
 * ============================================================================ */

/* NVIDIA cards expose several functions under vendor 0x10DE (GPU, HD Audio,
 * USB-C/xHCI, UCSI). Only the display function (class 0x03: VGA 03.00 or
 * 3D controller 03.02) counts as a "display adapter"; an NVIDIA xHCI
 * function still matches the xHCI filter on its own. */
uint32_t pci_classify(uint16_t vendor, uint8_t c, uint8_t s, uint8_t p)
{
    uint32_t m = 0;
    if (c == PCI_CLASS_STORAGE && s == PCI_SUBCLASS_SATA)              m |= PCI_MATCH_AHCI;
    if (c == PCI_CLASS_STORAGE && s == PCI_SUBCLASS_NVM)               m |= PCI_MATCH_NVME;
    if (vendor == PCI_VENDOR_NVIDIA && c == PCI_CLASS_DISPLAY)         m |= PCI_MATCH_NVIDIA_DISPLAY;
    if (c == PCI_CLASS_SERIAL_BUS && s == PCI_SUBCLASS_USB && p == PCI_PROGIF_XHCI)
                                                                       m |= PCI_MATCH_XHCI;
    return m;
}

void pci_log_device(const struct pci_device *d)
{
    out_s("pci: ");
    out_hex(d->bus, 2); out_c(':'); out_hex(d->device, 2); out_c('.'); out_hex(d->function, 1);
    out_c(' ');
    out_hex(d->vendor_id, 4); out_c(':'); out_hex(d->device_id, 4);
    out_s(" class "); out_hex(d->class_code, 2); out_c('.'); out_hex(d->subclass, 2);
    out_c('.'); out_hex(d->prog_if, 2);
    out_s(" rev "); out_hex(d->revision, 2);
    out_s(d->multifunction ? " MF " : "    ");
    out_s(pci_class_name(d->class_code, d->subclass, d->prog_if));
    if (d->interrupt_pin) {
        out_s(", INT"); out_c((char)('A' + d->interrupt_pin - 1));
        out_s("# IRQ "); out_dec(d->interrupt_line);
    }
    if (d->header_type == PCI_HEADER_PCI_BRIDGE) {
        out_s(", buses "); out_hex(d->secondary_bus, 2); out_c('-'); out_hex(d->subordinate_bus, 2);
    }
    for (uint32_t bit = 1; bit < (1u << PCI_MATCH_CATEGORIES); bit <<= 1)
        if (d->matches & bit) { out_s("  <"); out_s(pci_match_name((enum pci_match)bit)); out_c('>'); }
    out_c('\n');

    for (int i = 0; i < d->bar_count; i++) {
        const struct pci_bar *b = &d->bars[i];
        if (b->kind == PCI_BAR_UNUSED || b->kind == PCI_BAR_MEM64_UPPER) continue;
        out_s("            BAR"); out_dec((uint64_t)i); out_s(": ");
        switch (b->kind) {
        case PCI_BAR_IO:    out_s("I/O   port 0x"); out_hex_min(b->base); break;
        case PCI_BAR_MEM16: out_s("mem16 0x"); out_hex(b->base, 8); break;
        case PCI_BAR_MEM32: out_s("mem32 0x"); out_hex(b->base, 8); break;
        case PCI_BAR_MEM64: out_s("mem64 0x"); out_hex(b->base, 16); break;
        }
        if (b->prefetchable) out_s(" prefetchable");
        if (b->size) { out_s(" ("); out_size(b->size); out_c(')'); }
        out_c('\n');
    }
}

uint32_t pci_filter_and_log(struct pci_device *d, bool log)
{
    d->matches = pci_classify(d->vendor_id, d->class_code, d->subclass, d->prog_if);
    for (uint32_t bit = 1; bit < (1u << PCI_MATCH_CATEGORIES); bit <<= 1) {
        if (!(d->matches & bit)) continue;
        int k = match_index((enum pci_match)bit);
        bool already = false;
        for (size_t i = 0; i < g_match_count[k]; i++) if (g_matches[k][i] == d) already = true;
        if (already) continue;
        if (g_match_count[k] < PCI_MAX_MATCHES) g_matches[k][g_match_count[k]++] = d;
        else g_stats.match_overflow++;
    }
    if (log) pci_log_device(d);
    return d->matches;
}

/* ============================================================================
 *  BAR decoding and sizing
 * ============================================================================ */

/* Measure one BAR (or a 64-bit pair) by writing all-ones and reading back
 * the hard-wired zero bits. Decoding is switched off meanwhile so the
 * device never responds at the temporary all-ones address. */
static void size_bar(const struct pci_device *d, int i, struct pci_bar *b)
{
    uint8_t off = (uint8_t)(PCI_REG_BAR0 + 4 * i);
    irqstate_t s = irq_off();
    uint16_t cmd = (uint16_t)pci_read32(d->bus, d->device, d->function, PCI_REG_COMMAND);
    write_command(d->bus, d->device, d->function, (uint16_t)(cmd & ~(unsigned)(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE)));

    uint32_t lo = pci_read32(d->bus, d->device, d->function, off);
    pci_write32(d->bus, d->device, d->function, off, 0xFFFFFFFFu);
    uint32_t mask_lo = pci_read32(d->bus, d->device, d->function, off);
    pci_write32(d->bus, d->device, d->function, off, lo);

    uint32_t mask_hi = 0;
    if (b->kind == PCI_BAR_MEM64) {
        uint32_t hi = pci_read32(d->bus, d->device, d->function, (uint8_t)(off + 4));
        pci_write32(d->bus, d->device, d->function, (uint8_t)(off + 4), 0xFFFFFFFFu);
        mask_hi = pci_read32(d->bus, d->device, d->function, (uint8_t)(off + 4));
        pci_write32(d->bus, d->device, d->function, (uint8_t)(off + 4), hi);
    }

    write_command(d->bus, d->device, d->function, cmd);
    irq_restore_state(s);

    if (b->kind == PCI_BAR_IO) {
        uint32_t m = mask_lo & 0xFFFFFFFCu;
        if (m && !(m & 0xFFFF0000u)) m |= 0xFFFF0000u;     /* 16-bit I/O decoder */
        b->size = m ? (uint32_t)(~m + 1) : 0;
    } else if (b->kind == PCI_BAR_MEM64) {
        uint64_t m = ((uint64_t)mask_hi << 32) | (mask_lo & 0xFFFFFFF0u);
        b->size = m ? ~m + 1 : 0;
    } else {
        uint32_t m = mask_lo & 0xFFFFFFF0u;
        b->size = m ? (uint32_t)(~m + 1) : 0;
    }
    if (b->size == 0 && b->base == 0) b->kind = PCI_BAR_UNUSED;
}

static void decode_bars(struct pci_device *d)
{
    for (int i = 0; i < d->bar_count; i++) {
        struct pci_bar *b = &d->bars[i];
        uint32_t v = d->config.dword[4 + i];
        b->base = 0; b->size = 0; b->prefetchable = false;

        if (v & 0x1) {                                  /* I/O space */
            b->kind = PCI_BAR_IO;
            b->base = v & 0xFFFFFFFCu;
        } else {
            uint32_t type = (v >> 1) & 0x3;
            b->prefetchable = (v & 0x8) != 0;
            b->base = v & 0xFFFFFFF0u;
            if (type == 0x2 && i + 1 < d->bar_count) {  /* 64-bit: next slot is the high dword */
                b->kind = PCI_BAR_MEM64;
                b->base |= (uint64_t)d->config.dword[4 + i + 1] << 32;
            } else {
                b->kind = type == 0x1 ? PCI_BAR_MEM16 : PCI_BAR_MEM32;
            }
        }

        bool wide = b->kind == PCI_BAR_MEM64;
        if (g_flags & PCI_SCAN_SIZE_BARS) size_bar(d, i, b);
        else if (v == 0) b->kind = PCI_BAR_UNUSED;      /* unassigned or absent */

        if (wide) {                                     /* consume the upper slot */
            i++;
            d->bars[i].kind = PCI_BAR_MEM64_UPPER;
            d->bars[i].base = d->bars[i].size = 0;
            d->bars[i].prefetchable = false;
        }
    }
}

/* ============================================================================
 *  Function capture
 * ============================================================================ */
static struct pci_device *capture(uint8_t bus, uint8_t dev, uint8_t func, struct pci_device *parent)
{
    g_stats.functions_found++;
    if (g_count >= PCI_MAX_TRACKED) { g_stats.dropped++; return NULL; }

    struct pci_device *d = &g_pool[g_count++];
    for (int i = 0; i < 16; i++)
        d->config.dword[i] = pci_read32(bus, dev, func, (uint8_t)(i * 4));

    const struct pci_header_common *h = &d->config.common;
    d->bus = bus; d->device = dev; d->function = func;
    d->vendor_id     = h->vendor_id;
    d->device_id     = h->device_id;
    d->class_code    = h->class_code;
    d->subclass      = h->subclass;
    d->prog_if       = h->prog_if;
    d->revision      = h->revision_id;
    d->header_type   = h->header_type & PCI_HEADER_TYPE_MASK;
    d->multifunction = (h->header_type & PCI_HEADER_MULTIFUNC) != 0;
    d->parent        = parent;
    d->matches       = 0;
    d->subsystem_vendor_id = d->subsystem_id = 0;
    d->secondary_bus = d->subordinate_bus = 0;

    switch (d->header_type) {
    case PCI_HEADER_GENERAL:
        d->bar_count = 6;
        d->subsystem_vendor_id = d->config.type0.subsystem_vendor_id;
        d->subsystem_id        = d->config.type0.subsystem_id;
        d->interrupt_line      = d->config.type0.interrupt_line;
        d->interrupt_pin       = d->config.type0.interrupt_pin;
        break;
    case PCI_HEADER_PCI_BRIDGE:
        d->bar_count = 2;
        d->secondary_bus   = d->config.type1.secondary_bus;
        d->subordinate_bus = d->config.type1.subordinate_bus;
        d->interrupt_line  = d->config.type1.interrupt_line;
        d->interrupt_pin   = d->config.type1.interrupt_pin;
        break;
    default:                                    /* CardBus or unknown */
        d->bar_count = 0;
        d->interrupt_line = (uint8_t)(d->config.dword[15] & 0xFF);
        d->interrupt_pin  = (uint8_t)((d->config.dword[15] >> 8) & 0xFF);
        break;
    }
    decode_bars(d);
    pci_filter_and_log(d, (g_flags & PCI_SCAN_LOG) != 0);
    return d;
}

static bool is_pci_bridge(uint8_t class_code, uint8_t subclass, uint8_t header_type)
{
    return class_code == PCI_CLASS_BRIDGE &&
           (subclass == PCI_SUBCLASS_PCI_BRIDGE || subclass == PCI_SUBCLASS_PCI_BRIDGE_ST) &&
           (header_type & PCI_HEADER_TYPE_MASK) == PCI_HEADER_PCI_BRIDGE;
}

/* ============================================================================
 *  Recursive scan
 * ============================================================================ */
static void scan_bus(uint8_t bus, struct pci_device *parent, unsigned depth);

static void scan_function(uint8_t bus, uint8_t dev, uint8_t func,
                          struct pci_device *parent, unsigned depth)
{
    struct pci_device *d = capture(bus, dev, func, parent);

    /* Follow PCI-to-PCI bridges (PCIe root ports and switch ports are
     * type-1 bridges too). Read the IDs directly so that bridges are
     * still followed when the pool is full. */
    uint32_t class_reg = pci_read32(bus, dev, func, 0x08);
    uint8_t  hdr       = pci_read8(bus, dev, func, PCI_REG_HEADER_TYPE);
    if (is_pci_bridge((uint8_t)(class_reg >> 24), (uint8_t)(class_reg >> 16), hdr)) {
        g_stats.bridges_found++;
        uint8_t secondary = pci_read8(bus, dev, func, PCI_REG_SECONDARY_BUS);
        /* secondary == 0 means firmware never assigned a bus number; the
         * visited bitmap stops loops on misprogrammed hierarchies. */
        if (secondary != 0 && depth < MAX_BRIDGE_DEPTH)
            scan_bus(secondary, d, depth + 1);
    }
}

static void scan_device(uint8_t bus, uint8_t dev, struct pci_device *parent, unsigned depth)
{
    if (pci_read16(bus, dev, 0, PCI_REG_VENDOR_ID) == PCI_VENDOR_INVALID) return;

    scan_function(bus, dev, 0, parent, depth);

    /* Functions 1-7 exist only if function 0 sets the multi-function bit.
     * Probing them anyway produces "ghost" duplicates on some chipsets
     * that ignore the function number of single-function devices. */
    if (pci_read8(bus, dev, 0, PCI_REG_HEADER_TYPE) & PCI_HEADER_MULTIFUNC) {
        for (uint8_t func = 1; func < PCI_MAX_FUNCTIONS; func++)
            if (pci_read16(bus, dev, func, PCI_REG_VENDOR_ID) != PCI_VENDOR_INVALID)
                scan_function(bus, dev, func, parent, depth);
    }
}

static void scan_bus(uint8_t bus, struct pci_device *parent, unsigned depth)
{
    if (g_bus_visited[bus >> 3] & (1u << (bus & 7))) return;
    g_bus_visited[bus >> 3] |= (uint8_t)(1u << (bus & 7));
    g_stats.buses_scanned++;
    for (uint8_t dev = 0; dev < PCI_MAX_DEVICES; dev++)
        scan_device(bus, dev, parent, depth);
}

/* ============================================================================
 *  Brute-force scan: every bus, slot and function, no topology assumptions.
 *  Slower (8192 slot probes) but also finds root buses that are not reached
 *  through host-bridge function numbers (e.g. multi-root-complex systems).
 * ============================================================================ */
static void scan_brute_force(void)
{
    for (unsigned bus = 0; bus < PCI_MAX_BUSES; bus++) {
        bool any = false;
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICES; dev++) {
            if (pci_read16((uint8_t)bus, dev, 0, PCI_REG_VENDOR_ID) == PCI_VENDOR_INVALID) continue;
            any = true;
            capture((uint8_t)bus, dev, 0, NULL);
            if (pci_read8((uint8_t)bus, dev, 0, PCI_REG_HEADER_TYPE) & PCI_HEADER_MULTIFUNC)
                for (uint8_t func = 1; func < PCI_MAX_FUNCTIONS; func++)
                    if (pci_read16((uint8_t)bus, dev, func, PCI_REG_VENDOR_ID) != PCI_VENDOR_INVALID)
                        capture((uint8_t)bus, dev, func, NULL);
        }
        if (any) g_stats.buses_scanned++;
    }
    /* Reconstruct the parent links from the bridges' secondary bus numbers. */
    for (size_t i = 0; i < g_count; i++) {
        struct pci_device *d = &g_pool[i];
        if (is_pci_bridge(d->class_code, d->subclass, d->header_type)) g_stats.bridges_found++;
        if (d->bus == 0) continue;
        for (size_t j = 0; j < g_count; j++)
            if (g_pool[j].header_type == PCI_HEADER_PCI_BRIDGE && g_pool[j].secondary_bus == d->bus) {
                d->parent = &g_pool[j];
                break;
            }
    }
}

/* ============================================================================
 *  Public entry points
 * ============================================================================ */
static void reset_state(void)
{
    uint32_t *p = (uint32_t *)&g_stats;
    for (size_t i = 0; i < sizeof g_stats / 4; i++) p[i] = 0;
    for (size_t i = 0; i < sizeof g_bus_visited; i++) g_bus_visited[i] = 0;
    for (int k = 0; k < PCI_MATCH_CATEGORIES; k++) g_match_count[k] = 0;
    g_count = 0;
}

int pci_enumerate(enum pci_scan_mode mode, unsigned flags)
{
    reset_state();
    g_flags = flags;
    if (!pci_mech1_present()) {
        if (flags & PCI_SCAN_LOG) out_s("pci: configuration mechanism #1 not available\n");
        return -1;
    }
    if (flags & PCI_SCAN_LOG)
        out_s(mode == PCI_SCAN_RECURSIVE ? "pci: recursive scan from host bridge\n"
                                         : "pci: brute-force scan of 256 buses\n");

    if (mode == PCI_SCAN_BRUTE_FORCE) {
        scan_brute_force();
    } else if (pci_read8(0, 0, 0, PCI_REG_HEADER_TYPE) & PCI_HEADER_MULTIFUNC) {
        /* Several host controllers: function N of 00:00 owns root bus N. */
        for (uint8_t func = 0; func < PCI_MAX_FUNCTIONS; func++)
            if (pci_read16(0, 0, func, PCI_REG_VENDOR_ID) != PCI_VENDOR_INVALID)
                scan_bus(func, NULL, 0);
    } else {
        scan_bus(0, NULL, 0);
    }

    if (flags & PCI_SCAN_LOG) pci_log_summary();
    return (int)g_stats.functions_found;
}

void pci_log_summary(void)
{
    out_s("pci: "); out_dec(g_stats.functions_found); out_s(" functions on ");
    out_dec(g_stats.buses_scanned); out_s(" buses, "); out_dec(g_stats.bridges_found);
    out_s(" bridges, "); out_dec(g_stats.config_reads); out_s(" config reads\n");
    for (uint32_t bit = 1; bit < (1u << PCI_MATCH_CATEGORIES); bit <<= 1) {
        int k = match_index((enum pci_match)bit);
        out_s("pci:   "); out_s(pci_match_name((enum pci_match)bit)); out_s(": ");
        out_dec(g_match_count[k]);
        for (size_t i = 0; i < g_match_count[k]; i++) {
            const struct pci_device *d = g_matches[k][i];
            out_c(' ');
            out_hex(d->bus, 2); out_c(':'); out_hex(d->device, 2); out_c('.'); out_hex(d->function, 1);
        }
        out_c('\n');
    }
    if (g_stats.dropped)
        { out_s("pci: WARNING "); out_dec(g_stats.dropped); out_s(" functions not stored (raise PCI_MAX_TRACKED)\n"); }
    if (g_stats.match_overflow)
        { out_s("pci: WARNING "); out_dec(g_stats.match_overflow); out_s(" matches not stored (raise PCI_MAX_MATCHES)\n"); }
}

size_t pci_device_count(void) { return g_count; }

struct pci_device *pci_device_at(size_t i) { return i < g_count ? &g_pool[i] : NULL; }

struct pci_device *pci_find(uint8_t bus, uint8_t dev, uint8_t func)
{
    for (size_t i = 0; i < g_count; i++)
        if (g_pool[i].bus == bus && g_pool[i].device == dev && g_pool[i].function == func)
            return &g_pool[i];
    return NULL;
}

size_t pci_match_count(enum pci_match m)
{
    int k = match_index(m);
    return k < 0 ? 0 : g_match_count[k];
}

struct pci_device *pci_match_get(enum pci_match m, size_t i)
{
    int k = match_index(m);
    return (k < 0 || i >= g_match_count[k]) ? NULL : g_matches[k][i];
}

const struct pci_scan_stats *pci_stats(void) { return &g_stats; }

void pci_enable(struct pci_device *d, bool io, bool mmio, bool bus_master)
{
    uint16_t cmd = (uint16_t)pci_read32(d->bus, d->device, d->function, PCI_REG_COMMAND);
    if (io)         cmd |= PCI_CMD_IO_SPACE;
    if (mmio)       cmd |= PCI_CMD_MEM_SPACE;
    if (bus_master) cmd |= PCI_CMD_BUS_MASTER;
    write_command(d->bus, d->device, d->function, cmd);
    d->config.common.command = cmd;
}
