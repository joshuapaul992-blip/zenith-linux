/* kernel/power.c -- power off, restart and halt, on real hardware and VMs
 *
 * Power off is ACPI sleep state S5: the FADT gives the PM1a/PM1b control
 * ports (and how to switch the chipset from legacy to ACPI mode), and the
 * DSDT's \_S5_ package the SLP_TYP values to write with SLP_EN. The \_S5_
 * object is found by a byte scan of the AML for its name followed by a
 * package, the layout every firmware uses for it -- no AML interpreter.
 * If that is missing or does not work, the fixed ports of QEMU, Bochs and
 * VirtualBox are tried, then the machine halts with a message.
 *
 * Restart tries, in order: the FADT reset register, the PCI reset control
 * port 0xCF9, the 8042 keyboard controller's reset line, and finally a
 * triple fault (an empty IDT), which every x86 CPU turns into a reset.
 *
 * All three first flush the storage devices (NVMe shutdown) and stop the
 * other CPUs. */
#include <kernel/power.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/klog.h>
#include <kernel/smp.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/image.h>
#include <kernel/vfs.h>

/* FADT field offsets (ACPI 6.x, table 5.9) */
#define FADT_DSDT          40
#define FADT_SMI_CMD       48
#define FADT_ACPI_ENABLE   52
#define FADT_PM1A_CNT      64
#define FADT_PM1B_CNT      68
#define FADT_FLAGS         112
#define FADT_RESET_REG     116          /* generic address structure, 12 bytes */
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140
#define FADT_X_PM1A_CNT    172
#define FADT_RESET_REG_SUP (1u << 10)

#define SCI_EN  0x0001
#define SLP_EN  0x2000

struct sdt { char sig[4]; uint32_t length; };

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static void delay_ms(uint64_t ms)
{
    uint64_t t = time_ns();
    while (time_ns() - t < ms * 1000000ull) cpu_relax();
}

static void quiesce(const char *what)
{
    kprintf("power: %s\n", what);
    vfs_sync_all(true);                                 /* disk file systems: written back, marked clean */
    storage_shutdown();                                 /* NVMe: clean shutdown first */
    cli();
    smp_stop_others();                                  /* the other CPUs halt where they are */
}

/* SLP_TYPa/b of \_S5_ from the DSDT; false if not found. */
static bool find_s5(const uint8_t *dsdt, uint32_t len, uint8_t *a, uint8_t *b)
{
    for (uint32_t i = 36; i + 4 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4)) continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 1] == '\\' && dsdt[i - 2] == 0x08))) continue;  /* NameOp */
        uint32_t p = i + 4;
        if (p >= len || dsdt[p] != 0x12) continue;      /* PackageOp */
        p++;
        p += ((dsdt[p] >> 6) & 3) + 1;                  /* PkgLength */
        p++;                                            /* NumElements */
        uint8_t v[2];
        for (int k = 0; k < 2; k++) {
            if (p >= len) return false;
            if (dsdt[p] == 0x0A) p++;                   /* BytePrefix */
            if (p >= len) return false;
            v[k] = dsdt[p++] & 7;                       /* ZeroOp/OneOp are 0/1 themselves */
        }
        *a = v[0];
        *b = v[1];
        return true;
    }
    return false;
}

static void acpi_s5(void)
{
    const uint8_t *fadt = acpi_find_table("FACP");
    if (!fadt) { kprintf("power: no ACPI FADT\n"); return; }
    uint32_t flen = rd32(fadt + 4);
    uint64_t dsdt_phys = flen >= FADT_X_DSDT + 8 && rd64(fadt + FADT_X_DSDT) ? rd64(fadt + FADT_X_DSDT)
                                                                          : rd32(fadt + FADT_DSDT);
    const struct sdt *dsdt = acpi_map_table(dsdt_phys);
    uint8_t typa, typb;
    if (!dsdt || memcmp(dsdt->sig, "DSDT", 4) || !find_s5((const uint8_t *)dsdt, dsdt->length, &typa, &typb)) {
        kprintf("power: no \\_S5_ in the DSDT\n");
        return;
    }
    uint32_t pm1a = rd32(fadt + FADT_PM1A_CNT), pm1b = rd32(fadt + FADT_PM1B_CNT);
    if (!pm1a && flen >= FADT_X_PM1A_CNT + 12 && fadt[FADT_X_PM1A_CNT] == 1)    /* I/O space */
        pm1a = (uint32_t)rd64(fadt + FADT_X_PM1A_CNT + 4);
    if (!pm1a || pm1a > 0xFFFF) { kprintf("power: no PM1a control port\n"); return; }
    uint32_t smi = rd32(fadt + FADT_SMI_CMD);
    uint8_t enable = fadt[FADT_ACPI_ENABLE];
    if (!(inw((uint16_t)pm1a) & SCI_EN) && smi && enable) {    /* legacy mode: ask SMM to hand over */
        outb((uint16_t)smi, enable);
        for (int i = 0; i < 300 && !(inw((uint16_t)pm1a) & SCI_EN); i++) delay_ms(1);
    }
    kprintf("power: ACPI S5 (SLP_TYP %u/%u, PM1a %x, PM1b %x)\n", typa, typb, pm1a, pm1b);
    uint16_t va = (uint16_t)((inw((uint16_t)pm1a) & ~(0x1C00 | SLP_EN)) | (typa << 10));
    outw((uint16_t)pm1a, va);
    if (pm1b && pm1b <= 0xFFFF) {
        uint16_t vb = (uint16_t)((inw((uint16_t)pm1b) & ~(0x1C00 | SLP_EN)) | (typb << 10));
        outw((uint16_t)pm1b, vb);
        outw((uint16_t)pm1b, vb | SLP_EN);
    }
    outw((uint16_t)pm1a, va | SLP_EN);
    delay_ms(500);                                      /* the power should be gone by now */
}

/* The "safe to turn off" screen (assets/halt_screen.png), drawn straight
 * into the frame buffer over whatever was there (the X server too). */
static void final_screen(const char *msg)
{
    kprintf("%s\n", msg);
    image_draw_fullscreen(&halt_screen);
}

static void (*power_hook)(int what);

void power_set_hook(void (*fn)(int what)) { power_hook = fn; }

static void run_hook(int what)
{
    void (*hook)(int) = power_hook;
    power_hook = NULL;
    if (hook) hook(what);
}

void power_off(void)
{
    run_hook(POWER_HOOK_OFF);
    quiesce("powering off");
    acpi_s5();
    outw(0x604, 0x2000);                                /* QEMU q35/pc */
    outw(0xB004, 0x2000);                               /* Bochs, old QEMU */
    outw(0x4004, 0x3400);                               /* VirtualBox */
    delay_ms(100);
    final_screen("power: this machine did not power off; halted. It is now safe to turn it off.");
    cpu_halt_forever();
}

void power_reboot(void)
{
    run_hook(POWER_HOOK_REBOOT);
    quiesce("restarting");
    const uint8_t *fadt = acpi_find_table("FACP");
    if (fadt && rd32(fadt + 4) >= FADT_RESET_VALUE + 1 && (rd32(fadt + FADT_FLAGS) & FADT_RESET_REG_SUP)) {
        const uint8_t *g = fadt + FADT_RESET_REG;
        uint64_t addr = rd64(g + 4);
        uint8_t val = fadt[FADT_RESET_VALUE];
        if (g[0] == 1 && addr <= 0xFFFF) outb((uint16_t)addr, val);        /* system I/O */
        else if (g[0] == 0 && addr && addr < (4ull << 30)) *(volatile uint8_t *)(uintptr_t)addr = val;   /* memory */
        delay_ms(50);
    }
    outb(0xCF9, 0x02);                                  /* PCI reset control: full reset */
    delay_ms(1);
    outb(0xCF9, 0x06);
    delay_ms(50);
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);                                   /* 8042 reset line */
    delay_ms(50);
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) none = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(none));     /* triple fault: the CPU resets */
    cpu_halt_forever();
}

void power_halt(void)
{
    run_hook(POWER_HOOK_HALT);
    quiesce("halting");
    final_screen("System halted. It is now safe to turn off your computer.");
    cpu_halt_forever();
}
