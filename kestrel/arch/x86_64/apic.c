/* arch/x86_64/apic.c -- local APIC, IO APIC and the ACPI MADT
 *
 * The MADT ("APIC" table) lists the processors (their local APIC IDs), the
 * IO APICs (MMIO base, first global system interrupt) and the interrupt
 * source overrides: ISA IRQ n is GSI n unless an override says otherwise
 * (QEMU and most boards move the PIT from IRQ 0 to GSI 2), with the
 * polarity/trigger mode it gives. Registers are reached through the
 * identity map, made uncached.
 *
 * The local APIC timer runs in periodic mode at 1 kHz. Its input clock is
 * not architecturally known, so the boot CPU counts its ticks over 10 ms of
 * the calibrated HPET/TSC clock; the other CPUs reuse the result. */
#include <kernel/smp.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/klog.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/bootinfo.h>

/* local APIC registers (offsets) */
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LVT_MASKED      (1u << 16)
#define LVT_PERIODIC    (1u << 17)
#define ICR_PENDING     (1u << 12)
#define MSR_APIC_BASE   0x1B

struct madt {
    char     sig[4];
    uint32_t length;
    uint8_t  revision, checksum;
    char     oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
    uint32_t lapic_addr;
    uint32_t flags;                     /* bit 0: legacy PICs present */
    uint8_t  entries[];
} __attribute__((packed));

static volatile uint8_t *lapic;
static bool enabled;
static uint32_t timer_per_ms;           /* LAPIC timer counts per ms (divide by 16) */

static uint32_t cpu_apic_ids[MAX_CPUS];
static int ncpu_listed;

static struct ioapic { volatile uint32_t *base; uint32_t gsi_base, count; } ioapics[4];
static int nioapics;

static struct { bool set; uint32_t gsi; uint16_t flags; } iso[16];  /* ISA overrides */

static uint32_t lr(uint32_t reg) { return *(volatile uint32_t *)(lapic + reg); }
static void lw(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(lapic + reg) = v; (void)lr(LAPIC_ID); }

bool apic_enabled(void) { return enabled; }
int apic_cpu_count(void) { return ncpu_listed; }
uint32_t apic_cpu_id(int i) { return cpu_apic_ids[i]; }
uint32_t lapic_id(void) { return lapic ? lr(LAPIC_ID) >> 24 : 0; }
void lapic_eoi(void) { if (lapic) *(volatile uint32_t *)(lapic + LAPIC_EOI) = 0; }

static uint32_t io_read(struct ioapic *a, uint32_t reg) { a->base[0] = reg; return a->base[4]; }
static void io_write(struct ioapic *a, uint32_t reg, uint32_t v) { a->base[0] = reg; a->base[4] = v; }

static bool parse_madt(void)
{
    const struct madt *m = acpi_find_table("APIC");
    if (!m) { kprintf("apic: no MADT: staying on the 8259 PIC, one CPU\n"); return false; }
    uint64_t lapic_phys = m->lapic_addr;
    for (const uint8_t *p = m->entries; p + 2 <= (const uint8_t *)m + m->length;) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > (const uint8_t *)m + m->length) break;   /* malformed: stop */
        switch (type) {
        case 0:                                         /* processor local APIC */
            if (len >= 8) {
                uint32_t flags; memcpy(&flags, p + 4, 4);
                if ((flags & 3) && ncpu_listed < MAX_CPUS) cpu_apic_ids[ncpu_listed++] = p[3];
            }
            break;
        case 1:                                         /* IO APIC */
            if (len >= 12 && nioapics < 4) {
                uint32_t addr, gsi; memcpy(&addr, p + 4, 4); memcpy(&gsi, p + 8, 4);
                vmm_set_uncached(addr, 4096);
                ioapics[nioapics].base = (volatile uint32_t *)(uintptr_t)addr;
                ioapics[nioapics].gsi_base = gsi;
                ioapics[nioapics].count = ((io_read(&ioapics[nioapics], 1) >> 16) & 0xff) + 1;
                nioapics++;
            }
            break;
        case 2:                                         /* interrupt source override */
            if (len >= 10 && p[2] == 0 && p[3] < 16) {
                memcpy(&iso[p[3]].gsi, p + 4, 4);
                memcpy(&iso[p[3]].flags, p + 8, 2);
                iso[p[3]].set = true;
            }
            break;
        case 5:                                         /* 64-bit local APIC address */
            if (len >= 12) memcpy(&lapic_phys, p + 4, 8);
            break;
        }
        p += len;
    }
    if (!ncpu_listed || !nioapics) { kprintf("apic: MADT lists no CPU or no IO APIC: staying on the PIC\n"); return false; }
    if (lapic_phys >= (4ull << 30)) { kprintf("apic: local APIC above 4 GiB (not identity-mapped)\n"); return false; }
    vmm_set_uncached(lapic_phys, 4096);
    lapic = (volatile uint8_t *)(uintptr_t)lapic_phys;
    kprintf("apic: %d CPU(s) in the MADT, local APIC at %lx, %d IO APIC(s) (%u inputs at %p)\n", ncpu_listed,
            lapic_phys, nioapics, ioapics[0].count, (void *)ioapics[0].base);
    return true;
}

/* This CPU's local APIC: software-enable, accept everything, LINT1 = NMI. */
void lapic_init_cpu(void)
{
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | (1u << 11));    /* global enable */
    lw(LAPIC_TPR, 0);
    lw(LAPIC_LVT_LINT0, LVT_MASKED);                    /* the PIC is not used */
    lw(LAPIC_LVT_LINT1, 0x400);                         /* NMI */
    lw(LAPIC_LVT_ERR, VEC_SPURIOUS - 1);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_ESR, 0);
    lw(LAPIC_SVR, 0x100 | VEC_SPURIOUS);                /* enable, spurious vector */
    lapic_eoi();
}

void lapic_timer_start(void)
{
    if (!lapic) return;
    lw(LAPIC_TIMER_DIV, 0x3);                           /* divide by 16 */
    if (!timer_per_ms) {                                /* boot CPU: calibrate */
        lw(LAPIC_LVT_TIMER, LVT_MASKED);
        lw(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
        uint64_t t0 = time_ns();
        while (time_ns() - t0 < 10000000ull) cpu_relax();
        uint32_t used = 0xFFFFFFFFu - lr(LAPIC_TIMER_CUR);
        lw(LAPIC_TIMER_INIT, 0);
        timer_per_ms = used / 10;
        if (!timer_per_ms) timer_per_ms = 1;
        kprintf("apic: local APIC timer: %u counts per ms (bus clock / 16)\n", timer_per_ms);
    }
    lw(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | LVT_PERIODIC);
    lw(LAPIC_TIMER_INIT, timer_per_ms * 1000 / PIT_HZ);
}

bool lapic_icr_wait(void)
{
    for (int i = 0; i < 1000000; i++) {
        if (!(lr(LAPIC_ICR_LO) & ICR_PENDING)) return true;
        cpu_relax();
    }
    return false;
}

static void icr(uint32_t dest, uint32_t lo)
{
    uint64_t f = irq_save();
    lapic_icr_wait();
    lw(LAPIC_ICR_HI, dest << 24);
    lw(LAPIC_ICR_LO, lo);
    irq_restore(f);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) { if (lapic) icr(apic_id, vector); }

void lapic_send_nmi_all_others(void) { if (lapic) icr(0, 0xC0000 | 0x400); }   /* all-but-self, NMI */

/* INIT, then two STARTUPs at real-mode address page << 12 (Intel MP spec). */
void lapic_send_init_sipi(uint32_t apic_id, uint8_t page)
{
    icr(apic_id, 0x4500);                               /* INIT, level assert */
    uint64_t t = time_ns();
    while (time_ns() - t < 10000000ull) cpu_relax();    /* 10 ms */
    for (int i = 0; i < 2; i++) {
        icr(apic_id, 0x4600 | page);                    /* STARTUP */
        t = time_ns();
        while (time_ns() - t < 200000ull) cpu_relax();  /* 200 us */
    }
    lapic_icr_wait();
}

/* ---- IO APIC ---------------------------------------------------------------------- */
static struct ioapic *ioapic_for(uint32_t gsi, uint32_t *pin)
{
    for (int i = 0; i < nioapics; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].count) {
            *pin = gsi - ioapics[i].gsi_base;
            return &ioapics[i];
        }
    return NULL;
}

void ioapic_route_isa(int irq, uint8_t vector)
{
    uint32_t gsi = iso[irq].set ? iso[irq].gsi : (uint32_t)irq, pin;
    uint16_t fl = iso[irq].set ? iso[irq].flags : 0;
    struct ioapic *a = ioapic_for(gsi, &pin);
    if (!a) { kprintf("apic: no IO APIC input for IRQ %d (GSI %u)\n", irq, gsi); return; }
    uint32_t lo = vector;                               /* fixed delivery, physical destination */
    if ((fl & 3) == 3) lo |= 1u << 13;                  /* active low (ISA default: high) */
    if (((fl >> 2) & 3) == 3) lo |= 1u << 15;           /* level (ISA default: edge) */
    io_write(a, 0x10 + 2 * pin + 1, lapic_id() << 24);  /* to the boot CPU */
    io_write(a, 0x10 + 2 * pin, lo);
}

bool apic_init(void)
{
    if (strstr(g_boot.cmdline, "kestrel.noapic")) { kprintf("apic: disabled (kestrel.noapic)\n"); return false; }
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9))) { kprintf("apic: CPU has no local APIC\n"); return false; }
    if (!parse_madt()) return false;
    for (int i = 0; i < nioapics; i++)                  /* everything masked until routed */
        for (uint32_t p = 0; p < ioapics[i].count; p++) {
            io_write(&ioapics[i], 0x10 + 2 * p, LVT_MASKED);
            io_write(&ioapics[i], 0x10 + 2 * p + 1, 0);
        }
    outb(0x21, 0xFF);                                   /* 8259s: all masked from now on */
    outb(0xA1, 0xFF);
    lapic_init_cpu();
    enabled = true;
    return true;
}
