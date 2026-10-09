/* pci_sim_test.c -- host-side test of pci.c against a simulated config space
 *
 *   cc -std=gnu11 -O1 -g -fsanitize=address,undefined -DPCI_PORT_IO_HOOKS \
 *      -DPCI_NO_MEM_FALLBACK -I.. pci_sim_test.c ../pci.c -o pci_sim_test
 *
 * The simulator models Configuration Mechanism #1: a latch at 0xCF8 and a
 * data window at 0xCFC. BARs behave like hardware: writes keep only the
 * address bits the device decodes, so all-ones readback reveals the size.
 *
 * Topology:
 *   00:00.0  host bridge
 *   00:01.0  root port            -> bus 01
 *     01:00.0  switch upstream port -> bus 02
 *       02:00.0  NVIDIA GPU (MF)   VGA, BAR0 16 MiB, BAR1 64-bit 256 MiB pref,
 *                                  BAR3 64-bit 32 MiB pref, BAR5 I/O 128 B
 *       02:00.1  NVIDIA HD Audio   must NOT match "NVIDIA display"
 *       02:00.2  NVIDIA xHCI       must match xHCI only
 *   00:02.0  "ghost" device: answers every function number but is not MF
 *   00:03.0  QEMU xHCI             64-bit BAR0 16 KiB
 *   00:1c.0  root port            -> bus 04
 *     04:00.0  Samsung NVMe        64-bit BAR0 16 KiB
 *     04:01.0  broken bridge whose secondary bus points back at bus 04
 *   00:1d.0  bridge with secondary bus 0 (never configured by firmware)
 *   00:1f.0  ISA bridge (MF)
 *   00:1f.2  AHCI                  BAR4 I/O 32 B, BAR5 4 KiB
 *   00:1f.3  SMBus
 *   80:00.0  NVMe on an orphan root bus (only the brute-force scan finds it)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pci.h"

/* ---------------------------------------------------------------- simulator */
struct simfn {
    int bus, dev, func;
    uint8_t cfg[256];
    uint64_t bar_size[6];       /* 0 = unimplemented */
    int ghost;                  /* answers for all function numbers */
};

#define MAXFN 32
static struct simfn fns[MAXFN];
static int nfns;
static uint32_t latch;
static unsigned long writes_to_bars;

static void put16(uint8_t *c, int off, uint16_t v) { c[off] = v & 0xFF; c[off + 1] = v >> 8; }
static void put32(uint8_t *c, int off, uint32_t v) { for (int i = 0; i < 4; i++) c[off + i] = (v >> (8 * i)) & 0xFF; }
static uint32_t get32(const uint8_t *c, int off) { return c[off] | c[off+1] << 8 | c[off+2] << 16 | (uint32_t)c[off+3] << 24; }

static struct simfn *add(int bus, int dev, int func, uint16_t ven, uint16_t did,
                         uint8_t cls, uint8_t sub, uint8_t pif, uint8_t hdr)
{
    struct simfn *f = &fns[nfns++];
    memset(f, 0, sizeof *f);
    f->bus = bus; f->dev = dev; f->func = func;
    put16(f->cfg, 0x00, ven);
    put16(f->cfg, 0x02, did);
    put16(f->cfg, 0x04, 0x0007);            /* IO | MEM | bus master */
    put16(f->cfg, 0x06, 0x0010);            /* capabilities list (status) */
    f->cfg[0x08] = 0x01;                    /* revision */
    f->cfg[0x09] = pif; f->cfg[0x0A] = sub; f->cfg[0x0B] = cls;
    f->cfg[0x0E] = hdr;
    return f;
}

/* kind: 0 mem32, 2 mem64, 'i' io ; base must be size-aligned */
static void bar(struct simfn *f, int i, char kind, uint64_t base, uint64_t size, int pref)
{
    f->bar_size[i] = size;
    if (kind == 'i') {
        put32(f->cfg, 0x10 + 4 * i, (uint32_t)base | 1);
    } else if (kind == 2) {
        put32(f->cfg, 0x10 + 4 * i, (uint32_t)base | 0x4 | (pref ? 8 : 0));
        put32(f->cfg, 0x10 + 4 * (i + 1), (uint32_t)(base >> 32));
        f->bar_size[i + 1] = 0;
    } else {
        put32(f->cfg, 0x10 + 4 * i, (uint32_t)base | (pref ? 8 : 0));
    }
}

static void bridge_buses(struct simfn *f, int pri, int sec, int sub)
{
    f->cfg[0x18] = (uint8_t)pri; f->cfg[0x19] = (uint8_t)sec; f->cfg[0x1A] = (uint8_t)sub;
}

static struct simfn *lookup(uint32_t a)
{
    int bus = (a >> 16) & 0xFF, dev = (a >> 11) & 0x1F, func = (a >> 8) & 7;
    for (int i = 0; i < nfns; i++) {
        struct simfn *f = &fns[i];
        if (f->bus == bus && f->dev == dev && (f->func == func || f->ghost)) return f;
    }
    return NULL;
}

/* BAR write semantics: low type bits are read-only, address bits below the
 * size are hard-wired to 0. The upper dword of a 64-bit BAR is masked by the
 * high half of the size mask. */
static void bar_write(struct simfn *f, int i, uint32_t v)
{
    writes_to_bars++;
    int off = 0x10 + 4 * i;
    uint32_t old = get32(f->cfg, off);
    if (i > 0 && (get32(f->cfg, off - 4) & 0x7) == 0x4 && f->bar_size[i - 1]) {   /* upper half */
        uint64_t mask = ~(f->bar_size[i - 1] - 1);
        put32(f->cfg, off, v & (uint32_t)(mask >> 32));
        return;
    }
    uint64_t size = f->bar_size[i];
    if (!size) { put32(f->cfg, off, 0); return; }
    uint32_t lowbits = (old & 1) ? (old & 0x3) : (old & 0xF);
    uint32_t addrmask = (uint32_t)~(size - 1) & ((old & 1) ? 0xFFFFFFFCu : 0xFFFFFFF0u);
    if (old & 1) addrmask &= 0x0000FFFF;     /* 16-bit I/O decoder: upper bits read 0 */
    put32(f->cfg, off, (v & addrmask) | lowbits);
}

void pci_hook_outl(uint16_t port, uint32_t v)
{
    if (port == 0xCF8) { latch = v; return; }
    if (port != 0xCFC || !(latch & 0x80000000u)) return;
    struct simfn *f = lookup(latch);
    if (!f) return;
    int off = latch & 0xFC;
    int nbars = (f->cfg[0x0E] & 0x7F) == 0 ? 6 : (f->cfg[0x0E] & 0x7F) == 1 ? 2 : 0;
    if (off >= 0x10 && off < 0x10 + 4 * nbars) bar_write(f, (off - 0x10) / 4, v);
    else if (off == 0x04) put16(f->cfg, 0x04, (uint16_t)v);   /* command; status RW1C ignored */
}

uint32_t pci_hook_inl(uint16_t port)
{
    if (port == 0xCF8) return latch;
    if (port != 0xCFC) return 0xFFFFFFFFu;
    struct simfn *f = lookup(latch);
    return f ? get32(f->cfg, latch & 0xFC) : 0xFFFFFFFFu;
}

void pci_hook_outb(uint16_t port, uint8_t v) { (void)port; (void)v; }
uint8_t pci_hook_inb(uint16_t port) { (void)port; return 0x20; }

static void build_topology(void)
{
    nfns = 0;
    struct simfn *f;
    add(0, 0, 0, 0x8086, 0x29C0, 0x06, 0x00, 0x00, 0x00);                    /* host bridge */

    f = add(0, 1, 0, 0x8086, 0x29C1, 0x06, 0x04, 0x00, 0x01); bridge_buses(f, 0, 1, 2);
    f = add(1, 0, 0, 0x10B5, 0x8747, 0x06, 0x04, 0x00, 0x01); bridge_buses(f, 1, 2, 2);
    f = add(2, 0, 0, 0x10DE, 0x2684, 0x03, 0x00, 0x00, 0x80);                /* GPU, MF */
    bar(f, 0, 0, 0xF6000000, 16u << 20, 0);
    bar(f, 1, 2, 0x7C00000000ull, 256u << 20, 1);
    bar(f, 3, 2, 0x7C10000000ull, 32u << 20, 1);
    bar(f, 5, 'i', 0xE000, 128, 0);
    f->cfg[0x3C] = 11; f->cfg[0x3D] = 1;
    f = add(2, 0, 1, 0x10DE, 0x22BA, 0x04, 0x03, 0x00, 0x00);                /* HD audio */
    bar(f, 0, 0, 0xF7080000, 16u << 10, 0);
    f = add(2, 0, 2, 0x10DE, 0x1AD8, 0x0C, 0x03, 0x30, 0x00);                /* USB-C xHCI */
    bar(f, 0, 2, 0xF7040000, 256u << 10, 0);

    f = add(0, 2, 0, 0x1234, 0x1111, 0x08, 0x80, 0x00, 0x00); f->ghost = 1;  /* ghost */

    f = add(0, 3, 0, 0x1B36, 0x000D, 0x0C, 0x03, 0x30, 0x00);                /* xHCI */
    bar(f, 0, 2, 0xFEBF0000, 16u << 10, 0);

    f = add(0, 0x1C, 0, 0x8086, 0x2940, 0x06, 0x04, 0x00, 0x01); bridge_buses(f, 0, 4, 4);
    f = add(4, 0, 0, 0x144D, 0xA808, 0x01, 0x08, 0x02, 0x00);                /* NVMe */
    bar(f, 0, 2, 0xFE800000, 16u << 10, 0);
    f = add(4, 1, 0, 0xBAD0, 0x0001, 0x06, 0x04, 0x00, 0x01); bridge_buses(f, 4, 4, 4);  /* loop */

    f = add(0, 0x1D, 0, 0x8086, 0x2948, 0x06, 0x04, 0x00, 0x01); bridge_buses(f, 0, 0, 0);

    add(0, 0x1F, 0, 0x8086, 0x2918, 0x06, 0x01, 0x00, 0x80);                 /* ISA, MF */
    f = add(0, 0x1F, 2, 0x8086, 0x2922, 0x01, 0x06, 0x01, 0x00);             /* AHCI */
    bar(f, 4, 'i', 0xC040, 32, 0);
    bar(f, 5, 0, 0xFEBD5000, 4096, 0);
    f->cfg[0x3C] = 10; f->cfg[0x3D] = 1;
    add(0, 0x1F, 3, 0x8086, 0x2930, 0x0C, 0x05, 0x00, 0x00);                 /* SMBus */

    f = add(0x80, 0, 0, 0x144D, 0xA80A, 0x01, 0x08, 0x02, 0x00);             /* orphan NVMe */
    bar(f, 0, 2, 0x8000000000ull, 16u << 10, 0);
}

/* ------------------------------------------------------------------- checks */
static int failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void sink(char c) { putchar(c); }

__attribute__((unused)) static void common_checks(const char *mode)
{
    printf("-- checks (%s)\n", mode);
    struct pci_device *gpu = pci_find(2, 0, 0), *audio = pci_find(2, 0, 1), *usbc = pci_find(2, 0, 2);
    struct pci_device *ahci = pci_find(0, 0x1F, 2), *xhci = pci_find(0, 3, 0), *nvme = pci_find(4, 0, 0);
    CHECK(gpu && audio && usbc && ahci && xhci && nvme);
    if (!(gpu && audio && usbc && ahci && xhci && nvme)) return;

    /* filters */
    CHECK(pci_match_count(PCI_MATCH_AHCI) == 1 && pci_match_get(PCI_MATCH_AHCI, 0) == ahci);
    CHECK(pci_match_count(PCI_MATCH_NVIDIA_DISPLAY) == 1 && pci_match_get(PCI_MATCH_NVIDIA_DISPLAY, 0) == gpu);
    CHECK(audio->matches == 0);
    CHECK(usbc->matches == PCI_MATCH_XHCI);
    CHECK(pci_match_count(PCI_MATCH_XHCI) == 2);
    CHECK(pci_match_get(PCI_MATCH_XHCI, 5) == NULL);

    /* multi-function and ghost handling */
    int ghosts = 0;
    for (size_t i = 0; i < pci_device_count(); i++)
        if (pci_device_at(i)->vendor_id == 0x1234) ghosts++;
    CHECK(ghosts == 1);
    CHECK(gpu->multifunction && !ahci->multifunction);

    /* topology */
    CHECK(gpu->parent && gpu->parent->bus == 1 && gpu->parent->parent && gpu->parent->parent->device == 1);
    CHECK(nvme->parent && nvme->parent->device == 0x1C);
    CHECK(pci_find(0, 1, 0)->secondary_bus == 1 && pci_find(0, 1, 0)->subordinate_bus == 2);

    /* raw packed header copy */
    CHECK(gpu->config.common.vendor_id == 0x10DE && gpu->config.common.class_code == 0x03);
    CHECK(gpu->config.type0.interrupt_line == 11 && gpu->interrupt_pin == 1);
    CHECK(pci_find(1, 0, 0)->config.type1.secondary_bus == 2);

    /* BAR decoding + sizing */
    CHECK(gpu->bars[0].kind == PCI_BAR_MEM32 && gpu->bars[0].base == 0xF6000000 && gpu->bars[0].size == 16u << 20);
    CHECK(gpu->bars[1].kind == PCI_BAR_MEM64 && gpu->bars[1].base == 0x7C00000000ull &&
          gpu->bars[1].size == 256u << 20 && gpu->bars[1].prefetchable);
    CHECK(gpu->bars[2].kind == PCI_BAR_MEM64_UPPER);
    CHECK(gpu->bars[3].kind == PCI_BAR_MEM64 && gpu->bars[3].size == 32u << 20);
    CHECK(gpu->bars[5].kind == PCI_BAR_IO && gpu->bars[5].base == 0xE000 && gpu->bars[5].size == 128);
    CHECK(ahci->bars[5].kind == PCI_BAR_MEM32 && ahci->bars[5].base == 0xFEBD5000 && ahci->bars[5].size == 4096);
    CHECK(ahci->bars[4].kind == PCI_BAR_IO && ahci->bars[4].size == 32);
    CHECK(ahci->bars[0].kind == PCI_BAR_UNUSED);
    CHECK(nvme->bars[0].kind == PCI_BAR_MEM64 && nvme->bars[0].size == 16u << 10);

    /* sizing must restore every BAR and the command register */
    struct simfn *sf = NULL;
    for (int i = 0; i < nfns; i++) if (fns[i].bus == 2 && fns[i].dev == 0 && fns[i].func == 0) sf = &fns[i];
    CHECK(get32(sf->cfg, 0x10) == 0xF6000000u);
    CHECK(get32(sf->cfg, 0x14) == 0x0000000Cu && get32(sf->cfg, 0x18) == 0x7C);
    CHECK((get32(sf->cfg, 0x04) & 0xFFFF) == 0x0007);

    CHECK(pci_stats()->dropped == 0 && pci_stats()->match_overflow == 0);
}

#if PCI_MAX_TRACKED < 16
/* Built with a tiny pool: storage overflows, but the scan must still walk
 * every bridge and count every function. */
int main(void)
{
    build_topology();
    pci_set_log_sink(sink);
    printf("== pool overflow (PCI_MAX_TRACKED=%d) ==\n", PCI_MAX_TRACKED);
    int n = pci_enumerate(PCI_SCAN_RECURSIVE, PCI_SCAN_LOG);
    CHECK(n == 15);
    CHECK(pci_device_count() == PCI_MAX_TRACKED);
    CHECK(pci_stats()->dropped == 15 - PCI_MAX_TRACKED);
    CHECK(pci_stats()->buses_scanned == 4);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
#else
int main(void)
{
    build_topology();
    pci_set_log_sink(sink);

    printf("== recursive scan ==\n");
    int n = pci_enumerate(PCI_SCAN_RECURSIVE, PCI_SCAN_LOG | PCI_SCAN_SIZE_BARS);
    common_checks("recursive");
    CHECK(n == 15);                                     /* everything except bus 0x80 */
    CHECK(pci_match_count(PCI_MATCH_NVME) == 1);
    CHECK(pci_find(0x80, 0, 0) == NULL);
    CHECK(pci_stats()->buses_scanned == 4);             /* 00 01 02 04: loop + unassigned ignored */

    printf("\n== brute-force scan ==\n");
    build_topology();
    n = pci_enumerate(PCI_SCAN_BRUTE_FORCE, PCI_SCAN_LOG | PCI_SCAN_SIZE_BARS);
    common_checks("brute force");
    CHECK(n == 16);
    CHECK(pci_match_count(PCI_MATCH_NVME) == 2);
    CHECK(pci_find(0x80, 0, 0) && pci_find(0x80, 0, 0)->parent == NULL);

    printf("\n== without BAR sizing (read-only scan) ==\n");
    build_topology();
    writes_to_bars = 0;
    n = pci_enumerate(PCI_SCAN_RECURSIVE, 0);
    CHECK(n == 15 && writes_to_bars == 0);
    CHECK(pci_find(2, 0, 0)->bars[1].size == 0 && pci_find(2, 0, 0)->bars[1].base == 0x7C00000000ull);

    printf("\n== classifier edge cases ==\n");
    CHECK(pci_classify(0x10DE, 0x03, 0x02, 0x00) == PCI_MATCH_NVIDIA_DISPLAY);    /* 3D controller */
    CHECK(pci_classify(0x10DE, 0x04, 0x03, 0x00) == 0);
    CHECK(pci_classify(0x8086, 0x01, 0x06, 0x01) == PCI_MATCH_AHCI);
    CHECK(pci_classify(0x8086, 0x0C, 0x03, 0x20) == 0);                           /* EHCI */

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
#endif
