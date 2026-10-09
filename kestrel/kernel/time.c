/* kernel/time.c -- HPET / PIT-calibrated TSC clocksource (see time.h) */
#include <kernel/time.h>
#include <kernel/cpu.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/string.h>
#include <kernel/multiboot2.h>

enum source { SRC_NONE, SRC_HPET, SRC_TSC };
static enum source src;
static char src_name[64];

/* HPET */
static volatile uint8_t *hpet;
static uint64_t hpet_period_fs;          /* femtoseconds per tick */
static bool     hpet_wide;               /* 64-bit main counter   */
static uint64_t hpet_start, hpet_ext, hpet_last32;

/* TSC */
static uint64_t tsc_freq, tsc_start;

/* wall clock: CMOS RTC read once at boot, advanced by the clocksource */
static int64_t boot_epoch;

#define HPET_GCAP_ID    0x000
#define HPET_GEN_CONF   0x010
#define HPET_COUNTER    0x0F0

/* ======================================================================== */
/*  ACPI table lookup (RSDP from Multiboot2, else the BIOS search area)        */
/* ======================================================================== */
struct rsdp {
    char     sig[8];
    uint8_t  checksum;
    char     oem[6];
    uint8_t  revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t  xchecksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision, checksum;
    char     oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

struct acpi_hpet {
    struct sdt_header h;
    uint32_t block_id;
    uint8_t  space_id, bit_width, bit_offset, access_size;   /* generic address */
    uint64_t address;
    uint8_t  number;
    uint16_t min_tick;
    uint8_t  page_prot;
} __attribute__((packed));

static bool checksum_ok(const void *p, size_t n)
{
    const uint8_t *b = p; uint8_t s = 0;
    while (n--) s = (uint8_t)(s + *b++);
    return s == 0;
}

static const struct rsdp *find_rsdp(uintptr_t mbi)
{
    for (struct mb2_tag *t = mb2_first_tag(mbi); t->type != MB2_TAG_END; t = mb2_next_tag(t))
        if (t->type == MB2_TAG_ACPI_NEW || t->type == MB2_TAG_ACPI_OLD) {
            const struct rsdp *r = (const struct rsdp *)((uint8_t *)t + 8);
            if (!memcmp(r->sig, "RSD PTR ", 8) && checksum_ok(r, 20)) return r;
        }
    /* legacy BIOS: EBDA first KiB, then 0xE0000-0xFFFFF, 16-byte aligned */
    uintptr_t bda = 0x40E;                       /* BIOS data area: EBDA segment */
    __asm__("" : "+r"(bda));                     /* low memory is mapped: hide the constant from -Warray-bounds */
    uintptr_t ebda = (uintptr_t)(*(volatile uint16_t *)bda) << 4;
    uintptr_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int i = 0; i < 2; i++)
        for (uintptr_t a = ranges[i][0]; a && a < ranges[i][1]; a += 16) {
            const struct rsdp *r = (const struct rsdp *)a;
            if (!memcmp(r->sig, "RSD PTR ", 8) && checksum_ok(r, 20)) return r;
        }
    return NULL;
}

static const struct sdt_header *map_table(uint64_t phys)
{
    if (!phys) return NULL;
    if (phys + 4096 > PMM_MAX_PHYS) vmm_identity_map(phys, 4096, VMM_WRITE);
    const struct sdt_header *h = (const struct sdt_header *)(uintptr_t)phys;
    if (h->length > 4096 && phys + h->length > PMM_MAX_PHYS) vmm_identity_map(phys, h->length, VMM_WRITE);
    return h;
}

static const struct acpi_hpet *find_hpet(uintptr_t mbi)
{
    const struct rsdp *r = find_rsdp(mbi);
    if (!r) { kprintf("time: no ACPI RSDP found\n"); return NULL; }
    bool x = r->revision >= 2 && r->xsdt;
    const struct sdt_header *root = map_table(x ? r->xsdt : r->rsdt);
    if (!root || !checksum_ok(root, root->length)) { kprintf("time: ACPI root table invalid\n"); return NULL; }
    size_t n = (root->length - sizeof *root) / (x ? 8 : 4);
    const uint8_t *entries = (const uint8_t *)root + sizeof *root;
    for (size_t i = 0; i < n; i++) {
        uint64_t p = x ? *(const uint64_t *)(entries + i * 8) : *(const uint32_t *)(entries + i * 4);
        const struct sdt_header *h = map_table(p);
        if (h && !memcmp(h->sig, "HPET", 4) && checksum_ok(h, h->length))
            return (const struct acpi_hpet *)h;
    }
    return NULL;
}

/* ======================================================================== */
/*  HPET                                                                       */
/* ======================================================================== */
static uint64_t hpet_read(void)
{
    if (hpet_wide) return *(volatile uint64_t *)(hpet + HPET_COUNTER);
    /* 32-bit counter: extend in software; callers read far more often than
     * the ~42 s wrap period of a 100 MHz counter. */
    uint64_t f = irq_save();
    uint32_t now = *(volatile uint32_t *)(hpet + HPET_COUNTER);
    if (now < hpet_last32) hpet_ext += 1ull << 32;
    hpet_last32 = now;
    uint64_t v = hpet_ext | now;
    irq_restore(f);
    return v;
}

static bool hpet_init(uintptr_t mbi)
{
    const struct acpi_hpet *t = find_hpet(mbi);
    if (!t) return false;
    if (t->space_id != 0 || !t->address) { kprintf("time: HPET not memory mapped\n"); return false; }
    vmm_set_uncached(t->address, 4096);
    hpet = (volatile uint8_t *)(uintptr_t)t->address;

    uint64_t cap = *(volatile uint64_t *)(hpet + HPET_GCAP_ID);
    hpet_period_fs = cap >> 32;
    hpet_wide = (cap >> 13) & 1;
    if (hpet_period_fs == 0 || hpet_period_fs > 100000000ull) {   /* spec: <= 100 ns */
        kprintf("time: HPET period %lu fs invalid\n", hpet_period_fs);
        hpet = NULL;
        return false;
    }
    volatile uint64_t *conf = (volatile uint64_t *)(hpet + HPET_GEN_CONF);
    if (!(*conf & 1)) *conf |= 1;                   /* ENABLE_CNF (legacy routing untouched) */

    /* make sure it actually counts */
    uint64_t a = hpet_read();
    for (int i = 0; i < 100000 && hpet_read() == a; i++) cpu_relax();
    if (hpet_read() == a) { kprintf("time: HPET counter is stuck\n"); hpet = NULL; return false; }

    hpet_start = hpet_read();
    uint64_t khz = 1000000000000ull / hpet_period_fs;
    snprintf(src_name, sizeof src_name, "HPET %lu.%03lu MHz at 0x%lx (%s counter)",
             khz / 1000, khz % 1000, t->address, hpet_wide ? "64-bit" : "32-bit");
    return true;
}

/* ======================================================================== */
/*  TSC calibrated against PIT channel 2 (gate via port 0x61, OUT2 = bit 5)    */
/* ======================================================================== */
static uint64_t pit_measure_tsc(uint16_t ticks)
{
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x03) ));          /* gate low, speaker off */
    outb(0x43, 0xB0);                                /* ch2, lo/hi, mode 0, binary */
    outb(0x42, ticks & 0xFF);
    outb(0x42, ticks >> 8);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));    /* gate high: start counting */
    uint64_t t0 = rdtsc();
    uint64_t spins = 0;
    while (!(inb(0x61) & 0x20))                      /* OUT2 rises at terminal count */
        if (++spins > 50000000ull) return 0;        /* PIT channel 2 not working */
    uint64_t t1 = rdtsc();
    outb(0x61, p61);
    return t1 - t0;
}

static bool tsc_init(void)
{
    const uint16_t ticks = 11932;                    /* 10 ms at 1.193182 MHz */
    uint64_t best = 0;
    for (int i = 0; i < 5; i++) {                    /* minimum: least disturbed by SMIs */
        uint64_t d = pit_measure_tsc(ticks);
        if (d && (!best || d < best)) best = d;
    }
    if (!best) return false;
    tsc_freq = best * 1193182ull / ticks;
    tsc_start = rdtsc();
    snprintf(src_name, sizeof src_name, "TSC %lu.%03lu GHz (PIT-calibrated)",
             (unsigned long)(tsc_freq / 1000000000ull), (unsigned long)((tsc_freq / 1000000ull) % 1000));
    return true;
}

/* ======================================================================== */
/*  CMOS real-time clock (MC146818 compatible)                                 */
/* ======================================================================== */
static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

/* days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void rtc_snapshot(uint8_t r[6])
{
    for (int spin = 0; spin < 100000 && (cmos(0x0A) & 0x80); spin++) cpu_relax();   /* update in progress */
    static const uint8_t regs[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
    for (int i = 0; i < 6; i++) r[i] = cmos(regs[i]);
}

static int64_t rtc_read_epoch(void)
{
    uint8_t a[6], b[6];
    rtc_snapshot(a);
    for (int tries = 0; tries < 5; tries++) {           /* two equal reads: no rollover mid-read */
        rtc_snapshot(b);
        if (!memcmp(a, b, 6)) break;
        memcpy(a, b, 6);
    }
    uint8_t status_b = cmos(0x0B);
    bool pm = !(status_b & 0x02) && (a[2] & 0x80);
    a[2] &= 0x7F;
    if (!(status_b & 0x04))                             /* BCD -> binary */
        for (int i = 0; i < 6; i++) a[i] = (uint8_t)((a[i] & 0x0F) + (a[i] >> 4) * 10);
    if (!(status_b & 0x02)) { if (a[2] == 12) a[2] = 0; if (pm) a[2] = (uint8_t)(a[2] + 12); }
    unsigned sec = a[0], min = a[1], hour = a[2], day = a[3], mon = a[4];
    int64_t year = 2000 + a[5];
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60) {
        kprintf("time: RTC returned an invalid date, wall clock starts at the epoch\n");
        return 0;
    }
    return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

int64_t time_realtime_sec(void) { return boot_epoch + (int64_t)(time_ns() / 1000000000ull); }

void time_realtime(int64_t *sec, int64_t *nsec)
{
    uint64_t ns = time_ns();
    *sec = boot_epoch + (int64_t)(ns / 1000000000ull);
    *nsec = (int64_t)(ns % 1000000000ull);
}

/* ======================================================================== */
/*  public API                                                                 */
/* ======================================================================== */
void time_init(uintptr_t mbi)
{
    /* calibrate the TSC regardless: it backs udelay() when the HPET exists too */
    bool have_tsc = tsc_init();
    char tsc_desc[64];
    strlcpy(tsc_desc, src_name, sizeof tsc_desc);

    if (hpet_init(mbi)) src = SRC_HPET;
    else if (have_tsc) { src = SRC_TSC; strlcpy(src_name, tsc_desc, sizeof src_name); }
    else { src = SRC_NONE; strlcpy(src_name, "none (port 0x80 delays)", sizeof src_name); }

    boot_epoch = rtc_read_epoch();
    kprintf("time: RTC wall clock %ld (Unix seconds)\n", (long)boot_epoch);
    kprintf("time: clocksource %s%s%s\n", src_name, src == SRC_HPET && have_tsc ? "; " : "",
            src == SRC_HPET && have_tsc ? tsc_desc : "");
}

uint64_t time_ns(void)
{
    switch (src) {
    case SRC_HPET: {
        uint64_t ticks = hpet_read() - hpet_start;
        /* ticks * period_fs / 1e6, split to avoid 64-bit overflow */
        return (ticks / 1000000ull) * hpet_period_fs + (ticks % 1000000ull) * hpet_period_fs / 1000000ull;
    }
    case SRC_TSC: {
        uint64_t t = rdtsc() - tsc_start;
        return (t / tsc_freq) * 1000000000ull + (t % tsc_freq) * 1000000000ull / tsc_freq;
    }
    default:
        return 0;
    }
}

uint64_t time_us(void) { return time_ns() / 1000; }
uint64_t time_ms(void) { return time_ns() / 1000000; }

void udelay(uint32_t us)
{
    if (src == SRC_NONE) { while (us--) outb(0x80, 0); return; }
    uint64_t end = time_ns() + (uint64_t)us * 1000;
    while (time_ns() < end) cpu_relax();
}

void mdelay(uint32_t ms) { while (ms--) udelay(1000); }

const char *time_source(void) { return src_name; }
uint64_t tsc_hz(void) { return tsc_freq; }
