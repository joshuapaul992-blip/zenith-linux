/* =============================================================================
 *  ahci.c -- standalone AHCI SATA driver: HBA bring-up, port management,
 *            synchronous 48-bit LBA DMA reads/writes, MBR verify loop.
 *
 *  Reference: Serial ATA AHCI 1.3.1 specification, sections 10.1 (system
 *  software initialisation), 10.3 (port DMA engine control) and 6.2.2
 *  (non-queued error recovery); ATA/ATAPI Command Set (ACS-3) for commands.
 *
 *  Memory per port (allocated once, physically contiguous, zeroed):
 *      command list     1 KiB, 1 KiB aligned     (32 x 32-byte headers)
 *      received FIS     256 B, 256 B aligned
 *      command tables   NCS x 256 B, 128 B aligned (CFIS + 8 PRDs each)
 *      scratch          3 sectors, 4 KiB aligned (IDENTIFY, verify, selftest)
 * ============================================================================= */
#include "ahci.h"

/* ---------------------------------------------------------------- tunables */
#define T_STOP_US        500000u    /* PxCMD.CR / FR to clear (spec: 500 ms)  */
#define T_LINK_US        100000u    /* PxSSTS.DET to reach 3 after COMRESET   */
#define T_SIG_US         100000u    /* first D2H FIS (signature) to arrive    */
#define T_IDLE_US       5000000u    /* PxTFD.BSY/DRQ to clear (disk spin-up)  */
#define T_CMD_US       10000000u    /* single command completion              */
#define T_HANDOFF_US      25000u    /* BIOS to release ownership (spec 25 ms) */
#define T_BIOS_BUSY_US  2000000u    /* BIOS busy cleanup (spec 2 s)           */

/* ======================================================================== */
/*  low-level helpers                                                          */
/* ======================================================================== */
#define REG(x) (*(volatile uint32_t *)&(x))

static inline void mmio_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }
static inline void port_outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t port_inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

#ifndef AHCI_NO_MEM_FALLBACK
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{ unsigned char *d = dst; while (n--) *d++ = (unsigned char)c; return dst; }
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{ unsigned char *d = dst; const unsigned char *s = src; while (n--) *d++ = *s++; return dst; }
#endif

static void mem_zero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }
static void mem_fill(void *p, uint8_t v, size_t n) { uint8_t *b = p; while (n--) *b++ = v; }
static void mem_copy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static size_t mem_diff(const void *x, const void *y, size_t n)       /* count of differing bytes */
{
    const uint8_t *a = x, *b = y; size_t diff = 0;
    for (size_t i = 0; i < n; i++) diff += a[i] != b[i];
    return diff;
}

/* ======================================================================== */
/*  default platform hooks                                                     */
/* ======================================================================== */
static uint8_t g_pool[AHCI_STATIC_POOL] __attribute__((aligned(4096)));
static size_t  g_pool_used;

static void *default_dma_alloc(size_t size, size_t align, uint64_t *phys)
{
    size_t start = (g_pool_used + align - 1) & ~(align - 1);
    if (start + size > sizeof g_pool) return NULL;
    g_pool_used = start + size;
    *phys = (uint64_t)(uintptr_t)&g_pool[start];        /* identity mapped */
    return &g_pool[start];
}
static uint64_t default_virt_to_phys(const void *v) { return (uint64_t)(uintptr_t)v; }
static void *default_map_mmio(uint64_t phys, size_t size) { (void)size; return (void *)(uintptr_t)phys; }
static void default_delay_us(uint32_t us) { while (us--) port_outb(0x80, 0); }

static void com1_putc(char c)
{
    for (int spin = 0; spin < 100000 && !(port_inb(0x3F8 + 5) & 0x20); spin++)
        __asm__ volatile("pause");
    port_outb(0x3F8, (uint8_t)c);
}
static void default_log_putc(char c) { if (c == '\n') com1_putc('\r'); com1_putc(c); }

/* ======================================================================== */
/*  debug output                                                               */
/* ======================================================================== */
static void (*g_putc)(char) = default_log_putc;

static void out_c(char c) { g_putc(c); }
static void out_s(const char *s) { while (*s) out_c(*s++); }
static void out_hex(uint64_t v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) out_c(hex[(v >> (4 * i)) & 0xF]);
}
static void out_dec(uint64_t v)
{
    char buf[21]; int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_c(buf[--n]);
}
static void out_size(uint64_t bytes)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int i = 0; uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && i < 5) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; i++; }
    out_dec(whole);
    if (frac && whole < 100) { out_c('.'); out_dec(frac); }
    out_c(' '); out_s(u[i]);
}
static void out_port(int hba, int port)
{
    out_s("ahci");
    if (hba) { out_dec((uint64_t)hba); out_c('.'); } else out_c(' ');
    out_s("port "); out_dec((uint64_t)port); out_s(": ");
}

static void hexdump(const uint8_t *buf, uint32_t off, uint32_t len)
{
    for (uint32_t row = off; row < off + len; row += 16) {
        out_s("ahci:   "); out_hex(row, 4); out_s(": ");
        for (uint32_t i = 0; i < 16; i++) { out_hex(buf[row + i], 2); out_c(i == 7 ? '-' : ' '); }
        out_s(" |");
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t c = buf[row + i];
            out_c(c >= 0x20 && c < 0x7F ? (char)c : '.');
        }
        out_s("|\n");
    }
}

/* ======================================================================== */
/*  driver state                                                               */
/* ======================================================================== */
struct port_state {
    struct hba_state        *hba;
    struct ahci_port_regs   *regs;
    int                      num;
    bool                     ready;         /* engine running, disk identified */
    struct ahci_cmd_header  *clist;   uint64_t clist_phys;
    struct ahci_received_fis *fis;    uint64_t fis_phys;
    struct ahci_cmd_table   *tables;  uint64_t tables_phys;
    uint8_t                 *scratch; uint64_t scratch_phys; uint32_t scratch_sector;
};

struct hba_state {
    struct ahci_hba_regs *regs;
    uint64_t abar;
    uint32_t cap, cap2, vs, pi;
    uint32_t nslots;
    struct ahci_platform plat;
    struct port_state ports[32];
};

static struct hba_state g_hba[AHCI_MAX_HBAS];
static int              g_nhba;
static struct ahci_disk g_disks[AHCI_MAX_DISKS];
static int              g_ndisks;

static inline void delay_us(const struct hba_state *h, uint32_t us) { h->plat.delay_us(us); }

/* Poll `reg` until (value & mask) == want, or the timeout expires. */
static bool wait_reg(const struct hba_state *h, volatile uint32_t *reg, uint32_t mask,
                     uint32_t want, uint32_t timeout_us)
{
    for (uint32_t spin = 0; spin < 1000; spin++)        /* fast path, no delay */
        if ((*reg & mask) == want) return true;
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((*reg & mask) == want) return true;
        delay_us(h, 10);
    }
    return (*reg & mask) == want;
}

/* ======================================================================== */
/*  port engine control (AHCI 1.3.1 section 10.3)                              */
/* ======================================================================== */
static int port_stop(struct port_state *p)
{
    struct ahci_port_regs *r = p->regs;
    uint32_t cmd = REG(r->cmd);
    if (cmd & AHCI_PxCMD_ST) REG(r->cmd) = cmd & ~AHCI_PxCMD_ST;
    if (!wait_reg(p->hba, &REG(r->cmd), AHCI_PxCMD_CR, 0, T_STOP_US)) return AHCI_ERR_TIMEOUT;
    cmd = REG(r->cmd);
    if (cmd & AHCI_PxCMD_FRE) REG(r->cmd) = cmd & ~AHCI_PxCMD_FRE;
    if (!wait_reg(p->hba, &REG(r->cmd), AHCI_PxCMD_FR, 0, T_STOP_US)) return AHCI_ERR_TIMEOUT;
    return AHCI_OK;
}

/* COMRESET: hold SControl.DET = 1 for >= 1 ms, release, wait for the PHY. */
static bool port_comreset(struct port_state *p)
{
    struct ahci_port_regs *r = p->regs;
    uint32_t sctl = REG(r->sctl) & ~0xFFFu;
    REG(r->sctl) = sctl | AHCI_SCTL_IPM_NO_PS | AHCI_SCTL_DET_COMRESET;
    delay_us(p->hba, 2000);
    REG(r->sctl) = sctl | AHCI_SCTL_IPM_NO_PS;
    bool up = wait_reg(p->hba, &REG(r->ssts), AHCI_SSTS_DET_MASK, AHCI_SSTS_DET_PRESENT, T_LINK_US);
    REG(r->serr) = 0xFFFFFFFFu;
    return up;
}

/* Wait for BSY and DRQ to clear; kick the port with CLO or COMRESET if stuck. */
static bool port_wait_idle(struct port_state *p, uint32_t timeout_us)
{
    struct ahci_port_regs *r = p->regs;
    if (wait_reg(p->hba, &REG(r->tfd), AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, timeout_us)) return true;
    if (p->hba->cap & AHCI_CAP_SCLO) {
        REG(r->cmd) = REG(r->cmd) | AHCI_PxCMD_CLO;
        if (wait_reg(p->hba, &REG(r->cmd), AHCI_PxCMD_CLO, 0, T_STOP_US)) return true;
    }
    port_comreset(p);
    return wait_reg(p->hba, &REG(r->tfd), AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, timeout_us);
}

static int port_start(struct port_state *p)
{
    struct ahci_port_regs *r = p->regs;
    if (!port_wait_idle(p, T_IDLE_US)) return AHCI_ERR_TIMEOUT;
    uint32_t cmd = REG(r->cmd);
    if (!(cmd & AHCI_PxCMD_FRE)) { REG(r->cmd) = cmd | AHCI_PxCMD_FRE; cmd = REG(r->cmd); }
    REG(r->cmd) = cmd | AHCI_PxCMD_ST;
    return AHCI_OK;
}

/* Non-queued error recovery (section 6.2.2.1). */
static void port_recover(struct port_state *p)
{
    struct ahci_port_regs *r = p->regs;
    REG(r->cmd) = REG(r->cmd) & ~AHCI_PxCMD_ST;
    wait_reg(p->hba, &REG(r->cmd), AHCI_PxCMD_CR, 0, T_STOP_US);
    REG(r->serr) = 0xFFFFFFFFu;
    REG(r->is)   = 0xFFFFFFFFu;
    if (port_start(p) != AHCI_OK) {
        out_port((int)(p->hba - g_hba), p->num); out_s("recovery failed, port disabled\n");
        p->ready = false;
    }
}

/* ======================================================================== */
/*  command execution                                                          */
/* ======================================================================== */
struct seg { uint64_t phys; uint32_t bytes; };

static int find_slot(struct port_state *p)
{
    uint32_t busy = REG(p->regs->sact) | REG(p->regs->ci);
    for (uint32_t s = 0; s < p->hba->nslots; s++)
        if (!(busy & (1u << s))) return (int)s;
    return -1;
}

static int run_cmd(struct port_state *p, uint8_t ata_cmd, uint64_t lba, uint16_t count,
                   uint8_t device, bool write, const struct seg *segs, int nsegs)
{
    struct ahci_port_regs *r = p->regs;
    int slot = find_slot(p);
    if (slot < 0) return AHCI_ERR_BUSY;

    struct ahci_cmd_table *t = &p->tables[slot];
    mem_zero(t->cfis, sizeof t->cfis);
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)t->cfis;
    fis->fis_type   = FIS_TYPE_REG_H2D;
    fis->flags      = 0x80;                     /* C: this is a command */
    fis->command    = ata_cmd;
    fis->device     = device;
    fis->lba0       = (uint8_t)(lba);
    fis->lba1       = (uint8_t)(lba >> 8);
    fis->lba2       = (uint8_t)(lba >> 16);
    fis->lba3       = (uint8_t)(lba >> 24);
    fis->lba4       = (uint8_t)(lba >> 32);
    fis->lba5       = (uint8_t)(lba >> 40);
    fis->count_lo   = (uint8_t)(count);
    fis->count_hi   = (uint8_t)(count >> 8);

    for (int i = 0; i < nsegs; i++) {
        t->prdt[i].dba      = (uint32_t)segs[i].phys;
        t->prdt[i].dbau     = (uint32_t)(segs[i].phys >> 32);
        t->prdt[i].reserved = 0;
        t->prdt[i].dbc      = (segs[i].bytes - 1) & 0x3FFFFFu;
    }

    struct ahci_cmd_header *h = &p->clist[slot];
    h->flags = (uint16_t)(AHCI_CMDH_CFL(sizeof(struct fis_reg_h2d) / 4) | (write ? AHCI_CMDH_WRITE : 0));
    h->prdtl = (uint16_t)nsegs;
    h->prdbc = 0;

    if (!port_wait_idle(p, T_IDLE_US)) return AHCI_ERR_TIMEOUT;
    REG(r->is) = 0xFFFFFFFFu;
    mmio_barrier();                             /* table + header visible before CI */
    REG(r->ci) = 1u << slot;

    uint32_t bit = 1u << slot, is = 0, waited = 0;
    int err = AHCI_OK;
    for (uint32_t spin = 0;; spin++) {
        is = REG(r->is);
        if (is & AHCI_PxIS_TFES)                              { err = AHCI_ERR_IO;  break; }
        if (is & (AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)) { err = AHCI_ERR_HBA; break; }
        if (!(REG(r->ci) & bit)) break;
        if (spin > 2000) {                      /* stop spinning, start sleeping */
            if (waited >= T_CMD_US) { err = AHCI_ERR_TIMEOUT; break; }
            delay_us(p->hba, 10); waited += 10;
        }
    }
    mmio_barrier();
    uint32_t tfd = REG(r->tfd);
    if (err == AHCI_OK && (tfd & (AHCI_TFD_ERR | AHCI_TFD_DF))) err = AHCI_ERR_IO;

    if (err != AHCI_OK) {
        out_port((int)(p->hba - g_hba), p->num);
        out_s("command 0x"); out_hex(ata_cmd, 2); out_s(" LBA "); out_dec(lba);
        out_s(" failed ("); out_s(ahci_strerror(err));
        out_s("): PxIS=0x"); out_hex(is, 8); out_s(" status=0x"); out_hex(tfd & 0xFF, 2);
        out_s(" error=0x"); out_hex((tfd >> 8) & 0xFF, 2);
        out_s(" PxSERR=0x"); out_hex(REG(r->serr), 8); out_c('\n');
        port_recover(p);
    }
    return err;
}

/* ======================================================================== */
/*  IDENTIFY                                                                   */
/* ======================================================================== */
static void ata_string(char *dst, const uint16_t *words, int nwords)
{
    int n = 0;
    for (int i = 0; i < nwords; i++) {                 /* bytes are swapped per word */
        dst[n++] = (char)(words[i] >> 8);
        dst[n++] = (char)(words[i] & 0xFF);
    }
    dst[n] = 0;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0;
    int lead = 0;
    while (dst[lead] == ' ') lead++;
    if (lead) { int i = 0; while ((dst[i] = dst[i + lead]) != 0) i++; }
}

static int identify(struct port_state *p, struct ahci_disk *d)
{
    struct seg s = { p->scratch_phys, 512 };
    mem_zero(p->scratch, 512);
    int r = run_cmd(p, ATA_CMD_IDENTIFY, 0, 0, 0, false, &s, 1);
    if (r != AHCI_OK) return r;

    uint16_t *w = d->identify;
    mem_copy(w, p->scratch, 512);
    ata_string(d->serial, &w[10], 10);
    ata_string(d->firmware, &w[23], 4);
    ata_string(d->model, &w[27], 20);

    d->lba48 = (w[83] & (1u << 10)) && (w[86] & (1u << 10));
    d->sectors = d->lba48
        ? (uint64_t)w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32 | (uint64_t)w[103] << 48
        : (uint64_t)w[60] | (uint64_t)w[61] << 16;

    d->sector_size = 512;
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12)))   /* words 117-118 valid */
        d->sector_size = 2u * ((uint32_t)w[117] | (uint32_t)w[118] << 16);
    if (d->sector_size < 512 || d->sector_size > 65536 || (d->sector_size & 511))
        d->sector_size = 512;
    return d->sectors ? AHCI_OK : AHCI_ERR_NODEV;
}

/* ======================================================================== */
/*  port bring-up (AHCI 1.3.1 section 10.1.2, steps 3-7)                       */
/* ======================================================================== */
static void *alloc(struct hba_state *h, size_t size, size_t align, uint64_t *phys)
{
    void *v = h->plat.dma_alloc(size, align, phys);
    if (!v) return NULL;
    if ((*phys & (align - 1)) || (!(h->cap & AHCI_CAP_S64A) && *phys + size > 0x100000000ull))
        return NULL;                            /* misaligned, or unreachable by a 32-bit HBA */
    mem_zero(v, size);
    return v;
}

static void port_setup(struct hba_state *h, int n)
{
    struct port_state *p = &h->ports[n];
    struct ahci_port_regs *r = &h->regs->ports[n];
    int hi = (int)(h - g_hba);
    p->hba = h; p->regs = r; p->num = n; p->ready = false;

    /* 1. make sure the port is idle before touching CLB/FB */
    if (port_stop(p) != AHCI_OK) {
        out_port(hi, n); out_s("engine did not stop, issuing COMRESET\n");
        port_comreset(p);
        if (port_stop(p) != AHCI_OK) { out_port(hi, n); out_s("still running, port skipped\n"); return; }
    }

    /* 2. command list, received-FIS area and command tables */
    if (!p->clist) {
        p->clist  = alloc(h, 1024, 1024, &p->clist_phys);
        p->fis    = alloc(h, 256, 256, &p->fis_phys);
        p->tables = alloc(h, h->nslots * sizeof(struct ahci_cmd_table), 128, &p->tables_phys);
        if (!p->clist || !p->fis || !p->tables) {
            out_port(hi, n); out_s("out of DMA memory, port skipped\n");
            p->clist = NULL;
            return;
        }
    }
    REG(r->clb)  = (uint32_t)p->clist_phys;
    REG(r->clbu) = (uint32_t)(p->clist_phys >> 32);
    REG(r->fb)   = (uint32_t)p->fis_phys;
    REG(r->fbu)  = (uint32_t)(p->fis_phys >> 32);
    for (uint32_t s = 0; s < h->nslots; s++) {
        uint64_t ct = p->tables_phys + s * sizeof(struct ahci_cmd_table);
        p->clist[s].ctba  = (uint32_t)ct;
        p->clist[s].ctbau = (uint32_t)(ct >> 32);
    }

    /* 3. clear stale errors and interrupts; polled operation */
    REG(r->serr) = 0xFFFFFFFFu;
    REG(r->is)   = 0xFFFFFFFFu;
    REG(r->ie)   = 0;

    /* 4. power up / spin up, enable FIS reception */
    uint32_t cmd = REG(r->cmd) | AHCI_PxCMD_POD | AHCI_PxCMD_FRE;
    if (h->cap & AHCI_CAP_SSS) cmd |= AHCI_PxCMD_SUD;
    cmd = (cmd & ~AHCI_PxCMD_ICC_MASK) | AHCI_PxCMD_ICC_ACTIVE;
    REG(r->cmd) = cmd;

    /* 5. is anything attached? */
    if (!wait_reg(h, &REG(r->ssts), AHCI_SSTS_DET_MASK, AHCI_SSTS_DET_PRESENT, T_LINK_US)) {
        REG(r->cmd) = REG(r->cmd) & ~AHCI_PxCMD_FRE;
        return;                                 /* empty port: stay quiet */
    }
    REG(r->serr) = 0xFFFFFFFFu;                 /* link bring-up sets diagnostics bits */
    for (uint32_t t = 0; REG(r->sig) == 0xFFFFFFFFu && t < T_SIG_US; t += 100) delay_us(h, 100);

    uint32_t ssts = REG(r->ssts), sig = REG(r->sig);
    uint8_t speed = (uint8_t)((ssts >> AHCI_SSTS_SPD_SHIFT) & 0xF);
    if (sig != AHCI_SIG_ATA) {
        out_port(hi, n);
        out_s(sig == AHCI_SIG_ATAPI ? "ATAPI device (optical), not handled by this driver"
            : sig == AHCI_SIG_PM    ? "port multiplier, not supported"
            : sig == AHCI_SIG_SEMB  ? "enclosure management bridge, ignored"
            : "unknown signature");
        out_s(" [sig 0x"); out_hex(sig, 8); out_s("]\n");
        port_stop(p);
        return;
    }

    /* 6. start the command engine */
    if (port_start(p) != AHCI_OK) { out_port(hi, n); out_s("device stays busy, port skipped\n"); return; }

    /* 7. identify the disk */
    if (g_ndisks >= AHCI_MAX_DISKS) { out_port(hi, n); out_s("disk table full\n"); return; }
    struct ahci_disk *d = &g_disks[g_ndisks];
    mem_zero(d, sizeof *d);
    d->hba = hi; d->port = n; d->link_speed = speed;

    /* temporary 512-byte scratch for IDENTIFY, then the real per-sector one */
    if (!p->scratch) {
        p->scratch = alloc(h, 4096, 4096, &p->scratch_phys);
        if (!p->scratch) { out_port(hi, n); out_s("out of DMA memory\n"); return; }
    }
    p->ready = true;
    int rc = identify(p, d);
    if (rc != AHCI_OK) { out_port(hi, n); out_s("IDENTIFY failed\n"); p->ready = false; return; }
    if (d->sector_size * 3 > 4096) {            /* 4Kn and larger: bigger scratch */
        p->scratch = alloc(h, 3 * d->sector_size, 4096, &p->scratch_phys);
        if (!p->scratch) { out_port(hi, n); out_s("out of DMA memory\n"); p->ready = false; return; }
    }
    p->scratch_sector = d->sector_size;
    g_ndisks++;

    out_port(hi, n);
    out_s("SATA disk, ");
    out_s(speed == 1 ? "1.5" : speed == 2 ? "3.0" : speed == 3 ? "6.0" : "?"); out_s(" Gb/s, \"");
    out_s(d->model); out_s("\" serial "); out_s(d->serial); out_s(" fw "); out_s(d->firmware);
    out_c('\n');
    out_port(hi, n);
    out_dec(d->sectors); out_s(" sectors x "); out_dec(d->sector_size); out_s(" B = ");
    out_size(d->sectors * d->sector_size);
    out_s(d->lba48 ? ", 48-bit LBA\n" : ", 28-bit LBA only\n");
}

/* ======================================================================== */
/*  HBA bring-up (section 10.1.2, steps 1-2)                                   */
/* ======================================================================== */
static void bios_handoff(struct hba_state *h)
{
    struct ahci_hba_regs *r = h->regs;
    if (!(h->cap2 & AHCI_CAP2_BOH)) return;
    REG(r->bohc) = REG(r->bohc) | AHCI_BOHC_OOS;
    if (!wait_reg(h, &REG(r->bohc), AHCI_BOHC_BOS, 0, T_HANDOFF_US) || (REG(r->bohc) & AHCI_BOHC_BB))
        wait_reg(h, &REG(r->bohc), AHCI_BOHC_BB | AHCI_BOHC_BOS, 0, T_BIOS_BUSY_US);
    out_s("ahci: BIOS/OS handoff ");
    out_s(REG(r->bohc) & AHCI_BOHC_BOS ? "timed out (continuing)\n" : "complete\n");
}

int ahci_init(uint64_t abar, const struct ahci_platform *plat)
{
    if (g_nhba >= AHCI_MAX_HBAS) return AHCI_ERR_NOMEM;
    struct hba_state *h = &g_hba[g_nhba];
    mem_zero(h, sizeof *h);

    if (plat) h->plat = *plat;
    if (!h->plat.dma_alloc)    h->plat.dma_alloc    = default_dma_alloc;
    if (!h->plat.virt_to_phys) h->plat.virt_to_phys = default_virt_to_phys;
    if (!h->plat.map_mmio)     h->plat.map_mmio     = default_map_mmio;
    if (!h->plat.delay_us)     h->plat.delay_us     = default_delay_us;
    if (!h->plat.log_putc)     h->plat.log_putc     = default_log_putc;
    g_putc = h->plat.log_putc;

    if (!abar || (abar & 0x7FF)) {              /* ABAR is at least 2 KiB aligned */
        out_s("ahci: invalid ABAR 0x"); out_hex(abar, 16); out_c('\n');
        return AHCI_ERR_NODEV;
    }
    h->abar = abar;
    h->regs = h->plat.map_mmio(abar, sizeof(struct ahci_hba_regs));
    struct ahci_hba_regs *r = h->regs;

    h->cap  = REG(r->cap);
    h->cap2 = REG(r->cap2);
    h->vs   = REG(r->vs);
    h->nslots = ((h->cap >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;

    out_s("ahci: HBA "); out_dec((uint64_t)g_nhba); out_s(" at ABAR 0x"); out_hex(abar, abar >> 32 ? 16 : 8);
    out_s(", AHCI "); out_dec(h->vs >> 16); out_c('.'); out_dec((h->vs >> 8) & 0xFF);
    if (h->vs & 0xFF) { out_c('.'); out_dec(h->vs & 0xFF); }
    out_s(", "); out_dec((h->cap & AHCI_CAP_NP_MASK) + 1); out_s(" ports, ");
    out_dec(h->nslots); out_s(" slots");
    out_s(h->cap & AHCI_CAP_S64A ? ", 64-bit DMA" : ", 32-bit DMA");
    if (h->cap & AHCI_CAP_SNCQ) out_s(", NCQ");
    if (h->cap & AHCI_CAP_SSS)  out_s(", staggered spin-up");
    out_c('\n');

    /* Step 1: firmware handoff, then AHCI mode (GHC.AE) with interrupts off.
     * Ports are not reset here: the firmware's link state is kept and each
     * port is stopped individually, which is faster and avoids re-spinning
     * disks. A port that refuses to stop gets its own COMRESET. */
    bios_handoff(h);
    if (!(REG(r->ghc) & AHCI_GHC_AE)) REG(r->ghc) = REG(r->ghc) | AHCI_GHC_AE;
    REG(r->ghc) = (REG(r->ghc) | AHCI_GHC_AE) & ~AHCI_GHC_IE;
    if (!(REG(r->ghc) & AHCI_GHC_AE)) { out_s("ahci: HBA refused AHCI mode\n"); return AHCI_ERR_HBA; }

    /* Step 2: implemented ports */
    h->pi = REG(r->pi);
    if (!h->pi) h->pi = (uint32_t)((1ull << ((h->cap & AHCI_CAP_NP_MASK) + 1)) - 1);
    out_s("ahci: ports implemented 0x"); out_hex(h->pi, 8); out_c('\n');

    g_nhba++;
    int before = g_ndisks;
    for (int n = 0; n < 32; n++)
        if (h->pi & (1u << n)) port_setup(h, n);
    REG(r->is) = 0xFFFFFFFFu;                   /* clear global interrupt status */

    out_s("ahci: "); out_dec((uint64_t)(g_ndisks - before)); out_s(" disk(s) ready\n");
    return g_ndisks - before;
}

int ahci_disk_count(void) { return g_ndisks; }
struct ahci_disk *ahci_disk_get(int i) { return (i >= 0 && i < g_ndisks) ? &g_disks[i] : NULL; }

static struct port_state *port_of(struct ahci_disk *d)
{
    if (!d || d->hba < 0 || d->hba >= g_nhba) return NULL;
    struct port_state *p = &g_hba[d->hba].ports[d->port];
    return p->ready ? p : NULL;
}

/* ======================================================================== */
/*  block I/O                                                                  */
/* ======================================================================== */
static int transfer(struct ahci_disk *d, uint64_t lba, uint32_t count, uint8_t *buf, bool write)
{
    struct port_state *p = port_of(d);
    if (!p) return AHCI_ERR_NODEV;
    if (count == 0) return AHCI_OK;
    if (lba >= d->sectors || count > d->sectors - lba) return AHCI_ERR_RANGE;
    if ((uintptr_t)buf & 1) return AHCI_ERR_ALIGN;

    const struct hba_state *h = p->hba;
    const uint32_t ss = d->sector_size;
    const uint32_t max_secs = d->lba48 ? 0xFFFFu : 0x100u;
    const bool s64 = (h->cap & AHCI_CAP_S64A) != 0;

    while (count) {
        uint32_t want = count < max_secs ? count : max_secs;
        uint64_t want_bytes = (uint64_t)want * ss, got = 0;

        /* Build the PRDT: walk the buffer page by page, merging physically
         * contiguous pages, up to AHCI_PRDT_ENTRIES descriptors of <= 4 MiB. */
        struct seg segs[AHCI_PRDT_ENTRIES];
        int n = 0;
        while (got < want_bytes) {
            const uint8_t *va = buf + got;
            uint64_t pa = h->plat.virt_to_phys(va);
            uint64_t chunk = 4096 - ((uintptr_t)va & 4095);
            if (chunk > want_bytes - got) chunk = want_bytes - got;
            if (!s64 && pa + chunk > 0x100000000ull) return AHCI_ERR_ALIGN;
            if (n && segs[n - 1].phys + segs[n - 1].bytes == pa &&
                segs[n - 1].bytes + chunk <= AHCI_PRD_MAX_BYTES) {
                segs[n - 1].bytes += (uint32_t)chunk;
            } else {
                if (n == AHCI_PRDT_ENTRIES) break;
                segs[n].phys = pa; segs[n].bytes = (uint32_t)chunk; n++;
            }
            got += chunk;
        }
        /* A full PRDT may end mid-sector: trim to a whole number of sectors. */
        uint64_t bytes = got - got % ss, acc = 0;
        if (!bytes) return AHCI_ERR_ALIGN;
        for (int i = 0; i < n; i++) {
            if (acc + segs[i].bytes >= bytes) { segs[i].bytes = (uint32_t)(bytes - acc); n = i + 1; break; }
            acc += segs[i].bytes;
        }
        uint32_t secs = (uint32_t)(bytes / ss);

        int rc;
        if (d->lba48)
            rc = run_cmd(p, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT, lba,
                         (uint16_t)secs, ATA_DEVICE_LBA, write, segs, n);
        else                                    /* READ/WRITE DMA (28-bit), count 0 = 256 */
            rc = run_cmd(p, write ? 0xCA : 0xC8, lba & 0xFFFFFFu, (uint16_t)(secs & 0xFF),
                         (uint8_t)(ATA_DEVICE_LBA | ((lba >> 24) & 0x0F)), write, segs, n);
        if (rc != AHCI_OK) return rc;

        lba += secs; count -= secs; buf += bytes;
    }
    return AHCI_OK;
}

int ahci_read(struct ahci_disk *d, uint64_t lba, uint32_t count, void *buf)
{
    return transfer(d, lba, count, (uint8_t *)buf, false);
}

int ahci_write(struct ahci_disk *d, uint64_t lba, uint32_t count, const void *buf)
{
    return transfer(d, lba, count, (uint8_t *)(uintptr_t)buf, true);
}

int ahci_flush(struct ahci_disk *d)
{
    struct port_state *p = port_of(d);
    if (!p) return AHCI_ERR_NODEV;
    return run_cmd(p, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, ATA_DEVICE_LBA, false, NULL, 0);
}

const char *ahci_strerror(int e)
{
    switch (e) {
    case AHCI_OK:          return "ok";
    case AHCI_ERR_NODEV:   return "no device";
    case AHCI_ERR_TIMEOUT: return "timeout";
    case AHCI_ERR_IO:      return "device error";
    case AHCI_ERR_HBA:     return "host bus/interface error";
    case AHCI_ERR_RANGE:   return "LBA out of range";
    case AHCI_ERR_ALIGN:   return "buffer alignment/address";
    case AHCI_ERR_NOMEM:   return "out of memory";
    case AHCI_ERR_BUSY:    return "no free command slot";
    }
    return "unknown error";
}

/* ======================================================================== */
/*  debug: MBR verify loop                                                     */
/* ======================================================================== */
static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x05: case 0x0F: return "Extended";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    case 0xFD: return "Linux RAID";
    default:   return "other";
    }
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

int ahci_verify_mbr(struct ahci_disk *d, int iterations)
{
    struct port_state *p = port_of(d);
    if (!p) return AHCI_ERR_NODEV;
    if (iterations < 1) iterations = 1;
    uint32_t ss = d->sector_size;
    uint8_t *ref = p->scratch, *tmp = p->scratch + ss;

    out_port(d->hba, d->port);
    out_s("verify: reading MBR (LBA 0) x"); out_dec((uint64_t)iterations); out_c('\n');

    mem_fill(ref, 0xA5, ss);                    /* poison: stale data cannot pass */
    int rc = ahci_read(d, 0, 1, ref);
    if (rc != AHCI_OK) { out_s("ahci:   read failed: "); out_s(ahci_strerror(rc)); out_c('\n'); return rc; }

    int mismatches = 0;
    for (int i = 1; i < iterations; i++) {
        mem_fill(tmp, (uint8_t)(0x5A ^ i), ss);
        rc = ahci_read(d, 0, 1, tmp);
        if (rc != AHCI_OK) { out_s("ahci:   re-read failed: "); out_s(ahci_strerror(rc)); out_c('\n'); return rc; }
        size_t diff = mem_diff(ref, tmp, ss);
        if (diff) { mismatches++; out_s("ahci:   pass "); out_dec((uint64_t)i); out_s(": "); out_dec(diff); out_s(" bytes differ\n"); }
    }
    out_s("ahci:   "); out_dec((uint64_t)iterations); out_s(" reads, ");
    out_s(mismatches ? "INCONSISTENT data\n" : "all identical\n");

    out_s("ahci:   boot code start:\n");
    hexdump(ref, 0x000, 0x20);
    out_s("ahci:   disk signature + partition table + boot signature:\n");
    hexdump(ref, 0x1B0, 0x50);

    uint16_t sig = (uint16_t)(ref[510] | ref[511] << 8);
    bool valid = sig == 0xAA55;
    out_s("ahci:   MBR boot signature at 0x1FE: bytes "); out_hex(ref[510], 2); out_c(' ');
    out_hex(ref[511], 2); out_s(" (0x"); out_hex(sig, 4); out_s(") -> ");
    out_s(valid ? "VALID\n" : "MISSING (disk not partitioned with MBR)\n");
    if (!valid) return 1;

    out_s("ahci:   disk identifier 0x"); out_hex(le32(ref + 0x1B8), 8); out_c('\n');
    bool gpt = false;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = ref + 0x1BE + 16 * i;
        uint8_t type = e[4];
        if (!type) continue;
        uint32_t start = le32(e + 8), count = le32(e + 12);
        out_s("ahci:   partition "); out_dec((uint64_t)i + 1); out_s(": type 0x"); out_hex(type, 2);
        out_s(" ("); out_s(mbr_type_name(type)); out_c(')');
        if (e[0] == 0x80) out_s(" active");
        out_s(", start LBA "); out_dec(start); out_s(", "); out_dec(count); out_s(" sectors (");
        out_size((uint64_t)count * ss); out_s(")\n");
        if (type == 0xEE) gpt = true;
    }
    if (gpt && ahci_read(d, 1, 1, tmp) == AHCI_OK) {
        bool efi = tmp[0] == 'E' && tmp[1] == 'F' && tmp[2] == 'I' && tmp[3] == ' ' &&
                   tmp[4] == 'P' && tmp[5] == 'A' && tmp[6] == 'R' && tmp[7] == 'T';
        out_s(efi ? "ahci:   LBA 1 holds a GPT header (\"EFI PART\")\n"
                  : "ahci:   protective MBR but no GPT header at LBA 1\n");
    }
    return AHCI_OK;
}

/* ======================================================================== */
/*  debug: non-destructive write test                                          */
/* ======================================================================== */
int ahci_selftest_rw(struct ahci_disk *d)
{
    struct port_state *p = port_of(d);
    if (!p) return AHCI_ERR_NODEV;
    uint32_t ss = d->sector_size;
    uint64_t lba = d->sectors - 1;
    uint8_t *orig = p->scratch, *pat = p->scratch + ss, *back = p->scratch + 2 * ss;

    out_port(d->hba, d->port);
    out_s("selftest: write/read-back/restore at last LBA "); out_dec(lba);
    out_s(lba > 0x0FFFFFFFu ? " (needs 48-bit LBA)\n" : "\n");

    int rc = ahci_read(d, lba, 1, orig);
    if (rc) goto fail;
    out_s("ahci:   original sector starts with:\n");
    hexdump(orig, 0, 16);

    static const char tag[] = "KESTREL AHCI WRITE TEST, LBA=";
    mem_copy(pat, tag, sizeof tag - 1);
    for (unsigned i = 0; i < 8; i++) pat[sizeof tag - 1 + i] = (uint8_t)(lba >> (8 * i));
    uint64_t x = lba ^ 0x9E3779B97F4A7C15ull;
    for (uint32_t i = sizeof tag - 1 + 8; i < ss; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17; pat[i] = (uint8_t)x;
    }

    if ((rc = ahci_write(d, lba, 1, pat)) || (rc = ahci_flush(d))) goto fail;
    mem_fill(back, 0, ss);
    if ((rc = ahci_read(d, lba, 1, back))) goto fail;
    size_t diff1 = mem_diff(pat, back, ss);

    if ((rc = ahci_write(d, lba, 1, orig)) || (rc = ahci_flush(d))) goto fail;
    mem_fill(back, 0xFF, ss);
    if ((rc = ahci_read(d, lba, 1, back))) goto fail;
    size_t diff2 = mem_diff(orig, back, ss);

    out_s("ahci:   pattern read-back: "); out_s(diff1 ? "MISMATCH" : "match");
    out_s(", restore read-back: "); out_s(diff2 ? "MISMATCH" : "match"); out_c('\n');
    return (diff1 || diff2) ? AHCI_ERR_IO : AHCI_OK;
fail:
    out_s("ahci:   selftest aborted: "); out_s(ahci_strerror(rc)); out_c('\n');
    return rc;
}
