/* =============================================================================
 *  nvidia.c -- NVIDIA GPU probe, stage 1 (see nvidia.h)
 *
 *  Derived from nouveau (drivers/gpu/drm/nouveau/nvkm), Copyright Red Hat
 *  Inc., MIT licence: engine/device/base.c, subdev/fb/gp102.c,
 *  subdev/bios/shadow{,ramin,rom}.c, subdev/i2c/{auxgm200,padgm200,gm200}.c,
 *  engine/disp/{gf119,gm200}.c.
 * ============================================================================= */
#include "nvpriv.h"
#include <stdarg.h>

/* ======================================================================== */
/*  MMIO and small helpers                                                     */
/* ======================================================================== */
const struct nv_platform *nv_plat;
#define P       nv_plat
#define rd32    nv_rd32
#define wr32    nv_wr32
#define mask32  nv_mask
#define nvlog   nv_log
static void udelay(uint32_t us) { nv_udelay(us); }
static void mzero(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }
static void mcopy(void *dst, const void *src, size_t n) { uint8_t *a = dst; const uint8_t *b = src; while (n--) *a++ = *b++; }

void nv_log(const char *f, ...)
{
    struct nv_sbuf b = { 0, 0, 0, P->log_putc };
    va_list ap; va_start(ap, f); nv_sb_vprintf(&b, f, ap); va_end(ap);
}

const char *nv_strerror(int err)
{
    switch (err) {
    case NV_OK:          return "ok";
    case NV_ERR_NODEV:   return "no device";
    case NV_ERR_TIMEOUT: return "timeout";
    case NV_ERR_IO:      return "I/O error";
    case NV_ERR_NACK:    return "NACK";
    case NV_ERR_NOSINK:  return "no sink";
    case NV_ERR_NOMEM:   return "out of memory";
    case NV_ERR_NOBIOS:  return "no VBIOS";
    case NV_ERR_NOSUPP:  return "not supported";
    case NV_ERR_STATE:   return "unexpected hardware state";
    default:             return "error";
    }
}

/* ======================================================================== */
/*  chip identification (nouveau engine/device/base.c)                        */
/* ======================================================================== */
static const struct { uint16_t chipset; const char *name; } chips[] = {
    { 0x117, "GM107" }, { 0x118, "GM108" }, { 0x120, "GM200" }, { 0x124, "GM204" }, { 0x126, "GM206" },
    { 0x130, "GP100" }, { 0x132, "GP102" }, { 0x134, "GP104" }, { 0x136, "GP106" }, { 0x137, "GP107" },
    { 0x138, "GP108" }, { 0x140, "GV100" }, { 0x162, "TU102" }, { 0x164, "TU104" }, { 0x166, "TU106" },
    { 0x167, "TU117" }, { 0x168, "TU116" }, { 0x170, "GA100" }, { 0x172, "GA102" }, { 0x173, "GA103" },
    { 0x174, "GA104" }, { 0x176, "GA106" }, { 0x177, "GA107" },
};

static const char *family_name(uint16_t chipset)
{
    switch (chipset & 0x1f0) {
    case 0x050: case 0x080: case 0x090: case 0x0a0: return "Tesla";
    case 0x0c0: case 0x0d0: return "Fermi";
    case 0x0e0: case 0x0f0: case 0x100: return "Kepler";
    case 0x110: case 0x120: return "Maxwell";
    case 0x130: return "Pascal";
    case 0x140: return "Volta";
    case 0x160: return "Turing";
    case 0x170: return "Ampere";
    case 0x190: return "Ada";
    default:    return "unknown";
    }
}

static void identify(struct nv_device *d)
{
    d->boot0 = rd32(d, 0x000000);
    d->chipset = (uint16_t)((d->boot0 & 0x1ff00000) >> 20);
    d->chiprev = (uint8_t)(d->boot0 & 0xff);
    d->family = family_name(d->chipset);
    d->chip_name = "unknown";
    for (size_t i = 0; i < sizeof chips / sizeof *chips; i++)
        if (chips[i].chipset == d->chipset) d->chip_name = chips[i].name;

    if (d->chipset >= 0x130 && d->chipset < 0x190) {            /* gp102_fb_vidmem_size */
        uint32_t v = rd32(d, 0x100ce0);
        uint64_t size = (uint64_t)((v & 0x3f0) >> 4) << ((v & 0xf) + 20);
        d->vram_bytes = (v & 0x40000000) ? size / 16 * 15 : size;
    }
    d->display_present = d->chipset >= 0x110 ? !(rd32(d, 0x021c04) & 1) : true;
    d->aux_supported   = d->chipset >= 0x120 && d->chipset < 0x140;   /* gm200_i2c: GM20x, GP10x */
    d->state_supported = d->chipset >= 0x0d0 && d->chipset < 0x140;   /* gf119-style core channel state */
}

/* ======================================================================== */
/*  VBIOS shadow (nouveau bios/shadow*.c)                                     */
/* ======================================================================== */
struct source {
    const char *name;
    bool     (*init)(struct nv_device *d);
    uint32_t (*read)(struct nv_device *d, uint8_t *buf, uint32_t off, uint32_t len);
    void     (*fini)(struct nv_device *d);
};

static uint32_t saved_1700;

/* PRAMIN: the image the VBIOS left in VRAM, seen through the BAR0 window. */
static bool pramin_init(struct nv_device *d)
{
    if (d->chipset < 0x050 || !d->display_present) return false;
    uint32_t addr = rd32(d, 0x619f04);
    if (!(addr & 0x00000008) || (addr & 0x00000003) != 1) return false;   /* not enabled / not in VRAM */
    uint64_t a = (uint64_t)(addr & 0xffffff00) << 8;
    if (!a) a = ((uint64_t)rd32(d, 0x001700) << 16) + 0xf0000;
    saved_1700 = rd32(d, 0x001700);
    wr32(d, 0x001700, (uint32_t)(a >> 16));
    return true;
}
static uint32_t pramin_read(struct nv_device *d, uint8_t *buf, uint32_t off, uint32_t len)
{
    if (off + len > 0x100000) return 0;
    for (uint32_t i = off; i < off + len; i += 4) {
        uint32_t v = rd32(d, 0x700000 + i);
        buf[i] = (uint8_t)v; buf[i + 1] = (uint8_t)(v >> 8); buf[i + 2] = (uint8_t)(v >> 16); buf[i + 3] = (uint8_t)(v >> 24);
    }
    return len;
}
static void pramin_fini(struct nv_device *d) { wr32(d, 0x001700, saved_1700); }

/* PROM: the ROM chip through BAR0 0x300000, with the PCI ROM shadow off. */
static bool prom_init(struct nv_device *d) { mask32(d, 0x088050, 0x00000001, 0x00000000); return true; }
static uint32_t prom_read(struct nv_device *d, uint8_t *buf, uint32_t off, uint32_t len)
{
    if (off + len > 0x100000) return 0;
    for (uint32_t i = off; i < off + len; i += 4) {
        uint32_t v = rd32(d, 0x300000 + i);
        buf[i] = (uint8_t)v; buf[i + 1] = (uint8_t)(v >> 8); buf[i + 2] = (uint8_t)(v >> 16); buf[i + 3] = (uint8_t)(v >> 24);
    }
    return len;
}
static void prom_fini(struct nv_device *d) { mask32(d, 0x088050, 0x00000001, 0x00000001); }

/* PCIROM: the expansion ROM BAR, through the platform. */
static uint8_t *pcirom_copy;
static uint32_t pcirom_size;
static bool pcirom_init(struct nv_device *d)
{
    (void)d;
    if (!P->read_pci_rom || !(pcirom_copy = P->alloc(NV_BIOS_MAX))) return false;
    pcirom_size = P->read_pci_rom(pcirom_copy, NV_BIOS_MAX);
    return pcirom_size != 0;
}
static uint32_t pcirom_read(struct nv_device *d, uint8_t *buf, uint32_t off, uint32_t len)
{
    (void)d;
    if (off + len > pcirom_size) return 0;
    mcopy(buf + off, pcirom_copy + off, len);
    return len;
}
static void pcirom_fini(struct nv_device *d)
{
    (void)d;
    if (pcirom_copy && P->free) P->free(pcirom_copy, NV_BIOS_MAX);
    pcirom_copy = 0;
}

static const struct source sources[] = {
    { "PRAMIN", pramin_init, pramin_read, pramin_fini },
    { "PROM",   prom_init,   prom_read,   prom_fini   },
    { "PCIROM", pcirom_init, pcirom_read, pcirom_fini },
};

/* Read images progressively, like nouveau's shadow_image(): a header, then
 * the image it describes, then the next one. Returns the bytes read. */
static uint32_t shadow(struct nv_device *d, const struct source *s, uint8_t *buf)
{
    uint32_t have = 0, base = 0;
    for (int n = 0; n < NVBIOS_MAX_IMAGES; n++) {
        uint32_t want = base + 0x1000;
        if (want > NV_BIOS_MAX) break;
        if (want > have) { if (s->read(d, buf, have, want - have) != want - have) break; have = want; }
        struct nvbios_image im;
        if (!nvbios_image_header(buf, have, base, &im)) break;
        want = (base + im.size + 3) & ~3u;
        if (want > NV_BIOS_MAX) break;
        if (want > have) { if (s->read(d, buf, have, want - have) != want - have) break; have = want; }
        base += im.size;
        if (im.last) break;
    }
    return base;
}

static void read_vbios(struct nv_device *d)
{
    uint8_t *cand = P->alloc(NV_BIOS_MAX);
    if (!cand) return;
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        const struct source *s = &sources[i];
        /* like nouveau's "skip" for PCIROM: only when nothing better was found */
        if (s->read == pcirom_read && d->bios_score > 0) continue;
        if (!s->init(d)) { nvlog("nvidia: VBIOS from %s: not available\n", s->name); continue; }
        mzero(cand, NV_BIOS_MAX);
        uint32_t size = shadow(d, s, cand);
        s->fini(d);
        int score = 0;
        int n = size ? nvbios_images(cand, size, (struct nvbios_image[NVBIOS_MAX_IMAGES]){0}, NVBIOS_MAX_IMAGES, &score) : 0;
        nvlog("nvidia: VBIOS from %s: %u bytes, %d image(s), score %d\n", s->name, size, n, score);
        if (n && score > d->bios_score) {                       /* keep the best, as nouveau does */
            uint8_t *t = d->bios_data;
            d->bios_data = cand;
            d->bios_size = size;
            d->bios_score = score;
            d->bios_source = s->name;
            cand = t ? t : P->alloc(NV_BIOS_MAX);
            if (!cand) break;
        }
    }
    if (cand && cand != d->bios_data && P->free) P->free(cand, NV_BIOS_MAX);
    if (d->bios_data) d->bios_ok = nvbios_parse(&d->bios, d->bios_data, d->bios_size);
}

/* ======================================================================== */
/*  DP AUX, GM200 / GP10x (nouveau i2c/auxgm200.c, padgm200.c, gm200.c)        */
/* ======================================================================== */
#define AUX_TYPE_I2C_WR     0x0
#define AUX_TYPE_I2C_RD     0x1
#define AUX_TYPE_MOT        0x4
#define AUX_TYPE_NATIVE_WR  0x8
#define AUX_TYPE_NATIVE_RD  0x9

static void aux_autodpcd(struct nv_device *d, int ch, bool on)
{
    mask32(d, 0x00d968 + (uint32_t)ch * 0x50, 0x00010000, on ? 0x00010000 : 0);
}

static void aux_fini(struct nv_device *d, int ch) { mask32(d, 0x00d954 + (uint32_t)ch * 0x50, 0x00710000, 0); }

static int aux_init(struct nv_device *d, int ch)
{
    const uint32_t reg = 0x00d954 + (uint32_t)ch * 0x50, ureq = 0x00100000, urep = 0x01000000;
    uint32_t ctrl;
    int t = 1000;
    do {                                            /* previous transaction done */
        ctrl = rd32(d, reg); udelay(1);
        if (!t--) { nvlog("nvidia: aux %d: begin idle timeout %08x\n", ch, ctrl); return NV_ERR_TIMEOUT; }
    } while (ctrl & 0x07010000);
    mask32(d, reg, 0x00700000, ureq);
    t = 1000;
    do {
        ctrl = rd32(d, reg); udelay(1);
        if (!t--) { nvlog("nvidia: aux %d: magic wait %08x\n", ch, ctrl); aux_fini(d, ch); return NV_ERR_TIMEOUT; }
    } while ((ctrl & 0x07000000) != urep);
    return NV_OK;
}

/* One AUX transaction of up to 16 bytes. Returns <0 on error, otherwise the
 * reply code (0 = ACK; 1 = native NACK, 4 = I2C NACK). *size = bytes moved. */
int nv_aux_xfer(struct nv_device *d, int ch, bool retry, uint8_t type, uint32_t addr, uint8_t *data, uint8_t *size)
{
    const uint32_t base = (uint32_t)ch * 0x50;
    uint32_t ctrl, stat = 0, xbuf[4] = { 0 };
    int ret = aux_init(d, ch);
    if (ret < 0) goto out;

    stat = rd32(d, 0x00d958 + base);
    if (!(stat & 0x10000000)) { ret = NV_ERR_NOSINK; goto out; }

    aux_autodpcd(d, ch, false);
    if (!(type & 1)) {
        mcopy(xbuf, data, *size);
        for (int i = 0; i < 16; i += 4) wr32(d, 0x00d930 + base + (uint32_t)i, xbuf[i / 4]);
    }
    ctrl  = rd32(d, 0x00d954 + base);
    ctrl &= ~0x0001f1ffu;
    ctrl |= (uint32_t)type << 12;
    ctrl |= *size ? (uint32_t)(*size - 1) : 0x00000100;
    wr32(d, 0x00d950 + base, addr);

    int retries = 0;
    do {
        wr32(d, 0x00d954 + base, 0x80000000 | ctrl);           /* reset */
        wr32(d, 0x00d954 + base, 0x00000000 | ctrl);
        if (retries) udelay(400);
        wr32(d, 0x00d954 + base, 0x00010000 | ctrl);           /* go; up to 2 ms */
        int t = 2000;
        do {
            ctrl = rd32(d, 0x00d954 + base); udelay(1);
            if (!t--) { nvlog("nvidia: aux %d: timeout %08x\n", ch, ctrl); ret = NV_ERR_TIMEOUT; goto out_err; }
        } while (ctrl & 0x00010000);
        ret = 0;
        stat = mask32(d, 0x00d958 + base, 0, 0);
        if ((stat & 0x000f0000) == 0x00080000 || (stat & 0x000f0000) == 0x00020000) ret = 1;  /* defer */
        if (stat & 0x00000100) ret = NV_ERR_TIMEOUT;
        if (stat & 0x00000e00) ret = NV_ERR_IO;
    } while (ret && retry && retries++ < 32);

    if (ret >= 0 && (type & 1)) {
        for (int i = 0; i < 16; i += 4) xbuf[i / 4] = rd32(d, 0x00d940 + base + (uint32_t)i);
        mcopy(data, xbuf, *size);
        *size = (uint8_t)(stat & 0x1f);
    }
out_err:
    aux_autodpcd(d, ch, true);
out:
    aux_fini(d, ch);
    return ret < 0 ? ret : (int)((stat & 0x000f0000) >> 16);
}

/* Hybrid (shared I2C/AUX) pads must be switched to AUX mode first. */
void nv_aux_pad_mode(struct nv_device *d, int share)
{
    const uint32_t base = (uint32_t)share * 0x50;
    mask32(d, 0x00d970 + base, 0x0000c003, 0x00000002);
    mask32(d, 0x00d97c + base, 0x00000001, 0x00000000);
}

int nv_aux_dpcd_read(struct nv_device *d, int ch, uint32_t addr, uint8_t *buf, uint32_t len)
{
    while (len) {
        uint8_t n = (uint8_t)(len < 16 ? len : 16), got = n;
        int rc = nv_aux_xfer(d, ch, true, AUX_TYPE_NATIVE_RD, addr, buf, &got);
        if (rc < 0) return rc;
        if (rc) return NV_ERR_NACK;
        if (!got) return NV_ERR_IO;
        addr += got; buf += got; len -= got;
    }
    return NV_OK;
}

int nv_aux_dpcd_write(struct nv_device *d, int ch, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len) {
        uint8_t n = (uint8_t)(len < 16 ? len : 16), cnt = n, tmp[16];
        mcopy(tmp, buf, n);
        int rc = nv_aux_xfer(d, ch, true, AUX_TYPE_NATIVE_WR, addr, tmp, &cnt);
        if (rc < 0) return rc;
        if (rc) return NV_ERR_NACK;
        addr += n; buf += n; len -= n;
    }
    return NV_OK;
}

/* I2C over AUX: write the register offset, then read with the
 * "middle of transaction" bit until the last chunk (nouveau aux.c). */
int nv_aux_i2c_read(struct nv_device *d, int ch, uint8_t i2c, uint8_t offset, uint8_t *buf, uint32_t len)
{
    uint8_t one = 1, off = offset;
    int rc = nv_aux_xfer(d, ch, true, AUX_TYPE_I2C_WR | AUX_TYPE_MOT, i2c, &off, &one);
    if (rc < 0) return rc;
    if (rc) return NV_ERR_NACK;
    while (len) {
        uint8_t n = (uint8_t)(len < 16 ? len : 16), cnt = 0;
        uint8_t type = (uint8_t)(AUX_TYPE_I2C_RD | (len > 16 ? AUX_TYPE_MOT : 0));
        for (int tries = 0; tries < 32 && !cnt; tries++) {
            cnt = n;
            rc = nv_aux_xfer(d, ch, true, type, i2c, buf, &cnt);
            if (rc < 0) return rc;
            if (rc) return NV_ERR_NACK;
        }
        if (!cnt) return NV_ERR_IO;
        buf += cnt; len -= cnt;
    }
    return NV_OK;
}

static void probe_dp(struct nv_device *d)
{
    for (int i = 0; i < d->bios.noutputs; i++) {
        const struct nvbios_output *o = &d->bios.output[i];
        struct nv_dp_probe *p = &d->dp[i];
        if (o->type != DCB_OUTPUT_DP || o->location != 0 || o->i2c_index >= d->bios.ni2c) continue;
        const struct nvbios_i2c *c = &d->bios.i2c[o->i2c_index];
        if (c->auxch == DCB_I2C_UNUSED) continue;
        p->probed = true;
        p->aux = c->auxch;
        if (c->share != DCB_I2C_UNUSED) nv_aux_pad_mode(d, c->share);
        p->sink = rd32(d, 0x00d958 + (uint32_t)p->aux * 0x50) & 0x10000000;
        if (!p->sink) { p->dpcd_rc = p->edid_rc = NV_ERR_NOSINK; continue; }

        p->dpcd_rc = nv_aux_dpcd_read(d, p->aux, 0x000, p->dpcd, sizeof p->dpcd);
        if (p->dpcd_rc == NV_OK) {
            nv_aux_dpcd_read(d, p->aux, 0x100, p->link_cfg, sizeof p->link_cfg);
            nv_aux_dpcd_read(d, p->aux, 0x200, p->status, sizeof p->status);
        }
        p->edid_rc = nv_aux_i2c_read(d, p->aux, 0x50, 0, p->edid, 128);
        if (p->edid_rc == NV_OK) {
            p->edid_len = 128;
            if (p->edid[126] && nv_aux_i2c_read(d, p->aux, 0x50, 128, p->edid + 128, 128) == NV_OK)
                p->edid_len = 256;
        }
        nvlog("nvidia: DCB %d (DP, aux %d): sink, DPCD %s (rev %u.%u, max %u lanes x %u.%u Gbps), EDID %s\n",
             o->index, p->aux, nv_strerror(p->dpcd_rc), p->dpcd[0] >> 4, p->dpcd[0] & 0xf,
             p->dpcd[2] & 0x1f, p->dpcd[1] * 27 / 100, p->dpcd[1] * 27 % 100 / 10, nv_strerror(p->edid_rc));
    }
}

/* ======================================================================== */
/*  display engine state left by the firmware (nouveau disp/gf119.c, gm200.c) */
/* ======================================================================== */
static void read_display_state(struct nv_device *d)
{
    uint32_t caps = rd32(d, 0x612004);
    d->head_mask  = caps & 0x0000000f;
    d->sor_mask   = (caps & 0x0000ff00) >> 8;
    d->head_count = (int)rd32(d, 0x022448);

    for (int h = 0; h < NV_MAX_HEADS; h++) {
        if (!(d->head_mask & (1u << h))) continue;
        struct nv_head_state *s = &d->head[h];
        const uint32_t m = 0x640000 + (uint32_t)h * 0x300;    /* armed core-channel methods */
        uint32_t v;
        s->output_resource = rd32(d, m + 0x404);
        s->control = rd32(d, m + 0x408);
        v = rd32(d, m + 0x414); s->htotal = (uint16_t)v;       s->vtotal = (uint16_t)(v >> 16);
        v = rd32(d, m + 0x418); s->hsync_end = (uint16_t)v;    s->vsync_end = (uint16_t)(v >> 16);
        v = rd32(d, m + 0x41c); s->hblank_end = (uint16_t)v;   s->vblank_end = (uint16_t)(v >> 16);
        v = rd32(d, m + 0x420); s->hblank_start = (uint16_t)v; s->vblank_start = (uint16_t)(v >> 16);
        s->pixel_hz = rd32(d, m + 0x450);
        s->offset   = rd32(d, m + 0x460);
        s->size     = rd32(d, m + 0x468);
        s->storage  = rd32(d, m + 0x46c);
        s->params   = rd32(d, m + 0x470);
        s->viewport_in  = rd32(d, m + 0x4b8);
        s->viewport_out = rd32(d, m + 0x4c0);
        switch ((s->output_resource & 0x3c0) >> 6) {
        case 6: s->depth = 30; break;
        case 5: s->depth = 24; break;
        default: s->depth = 18; break;
        }
    }
    for (int i = 0; i < NV_MAX_SORS; i++) {
        if (!(d->sor_mask & (1u << i))) continue;
        struct nv_sor_state *s = &d->sor[i];
        s->ctrl  = rd32(d, 0x640200 + (uint32_t)i * 0x20);
        s->heads = s->ctrl & 0x0000000f;
        s->proto = (uint8_t)((s->ctrl & 0x00000f00) >> 8);
        for (int h = 0; h < NV_MAX_HEADS; h++)
            if ((s->heads & (1u << h)) && d->head[h].htotal) d->head[h].active = true;
    }
    /* gm200_sor_route_get: which SOR each DCB output is routed to */
    if (d->chipset >= 0x120)
        for (int i = 0; i < d->bios.noutputs; i++) {
            const struct nvbios_output *o = &d->bios.output[i];
            if (!o->or || o->location) continue;
            int orbit = 0;
            while (!(o->or & (1u << orbit))) orbit++;
            uint32_t v = rd32(d, 0x612308 + (uint32_t)orbit * 0x100 + ((o->link & 1) ? 0 : 0x80));
            d->dp[i].route_sor = (int)(v & 0x0f) - 1;
            d->dp[i].route_link = (int)((v & 0x10) >> 4);
        }
}

/* ======================================================================== */
/*  register snapshot for the modeset stage (reads only)                      */
/* ======================================================================== */
static void snap(struct nv_device *d, uint32_t reg, const char *what)
{
    if (d->nregs < NV_MAX_REGS) {
        d->regs[d->nregs].reg = reg;
        d->regs[d->nregs].val = rd32(d, reg);
        d->regs[d->nregs].what = what;
        d->nregs++;
    }
}

static void snapshot(struct nv_device *d)
{
    static const struct { uint32_t reg; const char *what; } global[] = {
        { 0x000200, "PMC_ENABLE" },          { 0x001700, "PRAMIN window" },
        { 0x001704, "BAR1 instance block" }, { 0x001714, "BAR2 instance block" },
        { 0x088050, "PCI ROM shadow" },      { 0x021c04, "display fuse" },
        { 0x022448, "head count" },          { 0x100ce0, "VRAM size" },
        { 0x610010, "display instance memory" },
        { 0x610078, "disp" },                { 0x61008c, "chan intr status" },
        { 0x610090, "chan intr enable" },    { 0x61009c, "chan error status" },
        { 0x6100a0, "chan error enable" },   { 0x6100ac, "supervisor/owner status" },
        { 0x6100b0, "supervisor intr enable" }, { 0x6101d0, "disp ctrl" },
        { 0x610490, "core channel control" }, { 0x611494, "core push" },
        { 0x611498, "core push 2" },         { 0x61149c, "core push 3" },
        { 0x612004, "head/DAC/SOR masks" },  { 0x6194e8, "vbios handoff" },
        { 0x619f04, "vbios image pointer" },
    };
    for (size_t i = 0; i < sizeof global / sizeof *global; i++) snap(d, global[i].reg, global[i].what);
    for (int h = 0; h < NV_MAX_HEADS; h++) {
        if (!(d->head_mask & (1u << h))) continue;
        const uint32_t o = (uint32_t)h * 0x800;
        snap(d, 0x616104 + o, "head caps 0"); snap(d, 0x616108 + o, "head caps 1"); snap(d, 0x61610c + o, "head caps 2");
        snap(d, 0x6101b4 + o, "head caps copy 0"); snap(d, 0x6101d4 + o, "head supervisor mask");
        snap(d, 0x6100c0 + o, "head vblank intr"); snap(d, 0x616308 + o, "head underflow");
        snap(d, 0x612200 + o, "head rgclk div");
        snap(d, 0x614100 + o, "VPLL ctrl"); snap(d, 0x614104 + o, "VPLL coef (P<<16|N<<8|M)");
        snap(d, 0x61410c + o, "VPLL 0c"); snap(d, 0x614110 + o, "VPLL fN");
    }
    for (int s = 0; s < NV_MAX_SORS; s++) {
        if (!(d->sor_mask & (1u << s))) continue;
        const uint32_t o = (uint32_t)s * 0x800;
        snap(d, 0x61c000 + o, "SOR caps"); snap(d, 0x6301c4 + o, "SOR caps copy");
        snap(d, 0x61c004 + o, "SOR power"); snap(d, 0x61c030 + o, "SOR state");
        snap(d, 0x612300 + o, "SOR clock"); snap(d, 0x61c10c + o, "SOR DP ctrl (link A)");
        snap(d, 0x61c110 + o, "SOR DP pattern"); snap(d, 0x61c118 + o, "SOR DP drive current");
        snap(d, 0x61c120 + o, "SOR DP pre-emphasis"); snap(d, 0x61c130 + o, "SOR DP 130");
        snap(d, 0x61c13c + o, "SOR DP post-cursor");
    }
    for (uint32_t m = 0; m < 16; m++) snap(d, 0x612308 + m * 0x80, "OR route (or*2 + sublink)");
}

/* ======================================================================== */
/*  probe and report                                                           */
/* ======================================================================== */
int nv_probe(struct nv_device *d, const struct nv_platform *plat)
{
    P = plat;
    d->mmio = P->map_mmio(d->bar0, 16u << 20);
    if (!d->mmio) return NV_ERR_NODEV;
    if (rd32(d, 0x000000) == 0xffffffffu) { nvlog("nvidia: BAR0 reads all ones\n"); return NV_ERR_NODEV; }
    identify(d);
    nvlog("nvidia: %04x:%04x is %s (%s, chipset %03x rev %02x), boot0 %08x, VRAM %lu MiB, display %s\n",
         d->vendor_id, d->device_id, d->chip_name, d->family, d->chipset, d->chiprev, d->boot0,
         (unsigned long)(d->vram_bytes >> 20), d->display_present ? "present" : "fused off");

    read_vbios(d);
    if (!d->bios_ok) { nvlog("nvidia: no usable VBIOS image\n"); return NV_ERR_NOBIOS; }
    nvlog("nvidia: VBIOS %02x.%02x.%02x.%02x.%02x from %s (%u bytes): DCB %u.%u, %d outputs, %d connectors, %d I2C/AUX ports\n",
         d->bios.version[0], d->bios.version[1], d->bios.version[2], d->bios.version[3], d->bios.version[4],
         d->bios_source, d->bios_size, d->bios.dcb_ver >> 4, d->bios.dcb_ver & 0xf, d->bios.noutputs,
         d->bios.nconns, d->bios.ni2c);

    for (int i = 0; i < NVBIOS_MAX_OUTPUTS; i++) { d->dp[i].aux = -1; d->dp[i].route_sor = -1; }
    if (d->display_present && d->state_supported) { read_display_state(d); snapshot(d); }
    if (d->display_present && d->aux_supported) probe_dp(d);
    else nvlog("nvidia: DP AUX probing not supported on %s yet\n", d->chip_name);
    return NV_OK;
}

static void hexline(struct nv_sbuf *b, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) nv_sb_printf(b, "%02x%s", p[i], i + 1 < n ? " " : "");
}

static const char *sor_proto(uint8_t p)
{
    switch (p) {
    case 0: return "LVDS"; case 1: return "TMDS-A"; case 2: return "TMDS-B"; case 5: return "TMDS dual";
    case 8: return "DP-A"; case 9: return "DP-B"; default: return "?";
    }
}

size_t nv_report(const struct nv_device *d, char *buf, size_t cap)
{
    struct nv_sbuf b = { buf, 0, cap, 0 };
    nv_sb_printf(&b, "PCI        %02x:%02x.%x  %04x:%04x  subsystem %04x:%04x\n", d->bus, d->dev, d->fn,
              d->vendor_id, d->device_id, d->subsys_vendor, d->subsys_id);
    nv_sb_printf(&b, "BARs       BAR0 %lx (MMIO), BAR1 %lx (%lu MiB VRAM aperture)\n",
              (unsigned long)d->bar0, (unsigned long)d->bar1, (unsigned long)(d->bar1_size >> 20));
    nv_sb_printf(&b, "chip       %s (%s), chipset %03x rev %02x, PMC_BOOT_0 %08x\n", d->chip_name, d->family,
              d->chipset, d->chiprev, d->boot0);
    nv_sb_printf(&b, "VRAM       %lu MiB\n", (unsigned long)(d->vram_bytes >> 20));
    nv_sb_printf(&b, "display    %s; AUX probing %s; firmware state readback %s\n",
              d->display_present ? "present" : "fused off", d->aux_supported ? "yes" : "no (chip not supported yet)",
              d->state_supported ? "yes" : "no");
    if (!d->bios_ok) { nv_sb_printf(&b, "VBIOS      none found\n"); return b.n < cap ? b.n : cap; }

    const struct nvbios *v = &d->bios;
    nv_sb_printf(&b, "VBIOS      %02x.%02x.%02x.%02x.%02x, %u bytes from %s (score %d), BIT at %x\n",
              v->version[0], v->version[1], v->version[2], v->version[3], v->version[4], d->bios_size,
              d->bios_source, d->bios_score, v->bit_offset);
    for (int i = 0; i < v->nimages; i++)
        nv_sb_printf(&b, "  image %d  %06x +%6u bytes, type %02x%s%s, PCI %04x:%04x\n", i, v->image[i].base, v->image[i].size,
                  v->image[i].type, v->image[i].type == 0 ? (v->image[i].checksum_ok ? ", checksum ok" : ", BAD CHECKSUM") : "",
                  v->image[i].last ? ", last" : "", v->image[i].vendor, v->image[i].device);
    nv_sb_printf(&b, "DCB        version %u.%u at %x, %u entries; connector table %x (v%x), I2C table %x (v%x)\n",
              v->dcb_ver >> 4, v->dcb_ver & 0xf, v->dcb, v->dcb_cnt, v->conn_table, v->conn_ver, v->i2c_table, v->i2c_ver);

    nv_sb_printf(&b, "\nConnectors\n");
    for (int i = 0; i < v->nconns; i++)
        nv_sb_printf(&b, "  %d: %-12s type %02x, location %u, hpd %02x, dp %02x\n", i, nvbios_conn_type_name(v->conn[i].type),
                  v->conn[i].type, v->conn[i].location, v->conn[i].hpd, v->conn[i].dp);
    nv_sb_printf(&b, "\nI2C / AUX ports\n");
    for (int i = 0; i < v->ni2c; i++) {
        const struct nvbios_i2c *c = &v->i2c[i];
        if (c->type == DCB_I2C_UNUSED) { nv_sb_printf(&b, "  %d: unused\n", i); continue; }
        nv_sb_printf(&b, "  %d: type %02x, i2c port %d, aux channel %d, pad %d\n", i, c->type,
                  c->drive == 0xff ? -1 : c->drive, c->auxch == 0xff ? -1 : c->auxch, c->share == 0xff ? -1 : c->share);
    }

    nv_sb_printf(&b, "\nOutputs (DCB) and what is attached\n");
    for (int i = 0; i < v->noutputs; i++) {
        const struct nvbios_output *o = &v->output[i];
        const struct nv_dp_probe *p = &d->dp[i];
        nv_sb_printf(&b, "  DCB %d: %-6s SOR mask %x, link %x, heads %x, connector %u, i2c %u, location %u  [%08x %08x]\n",
                  o->index, nvbios_output_type_name(o->type), o->or, o->link, o->heads, o->connector, o->i2c_index,
                  o->location, o->raw_conn, o->raw_conf);
        if (o->type == DCB_OUTPUT_DP)
            nv_sb_printf(&b, "         board limit %u lanes x %u.%02u Gbps\n", o->dp_link_nr, o->dp_link_bw * 27 / 100, o->dp_link_bw * 27 % 100);
        if (p->route_sor >= 0) nv_sb_printf(&b, "         routed to SOR %d (link %d)\n", p->route_sor, p->route_link);
        if (!p->probed) continue;
        if (!p->sink) { nv_sb_printf(&b, "         aux %d: nothing connected\n", p->aux); continue; }
        nv_sb_printf(&b, "         aux %d: sink present; DPCD: %s\n", p->aux, nv_strerror(p->dpcd_rc));
        if (p->dpcd_rc == NV_OK) {
            nv_sb_printf(&b, "           caps   "); hexline(&b, p->dpcd, 16); nv_sb_printf(&b, "\n");
            nv_sb_printf(&b, "           DPCD %u.%u, max %u lanes x %u.%02u Gbps%s; firmware trained %u lanes x %u.%02u Gbps\n",
                      p->dpcd[0] >> 4, p->dpcd[0] & 0xf, p->dpcd[2] & 0x1f, p->dpcd[1] * 27 / 100, p->dpcd[1] * 27 % 100,
                      (p->dpcd[2] & 0x80) ? ", enhanced framing" : "", p->link_cfg[1] & 0x1f,
                      p->link_cfg[0] * 27 / 100, p->link_cfg[0] * 27 % 100);
            nv_sb_printf(&b, "           status "); hexline(&b, p->status, 6); nv_sb_printf(&b, "  (sink count, IRQ, lane 0-3 status, align)\n");
        }
        nv_sb_printf(&b, "         EDID: %s", nv_strerror(p->edid_rc));
        if (p->edid_rc == NV_OK && p->edid_len >= 128) {
            const uint8_t *e = p->edid;
            uint16_t id = (uint16_t)(e[8] << 8 | e[9]);
            int w = e[56] | (e[58] & 0xf0) << 4, h = e[59] | (e[61] & 0xf0) << 4;
            nv_sb_printf(&b, ", %c%c%c %04x, preferred %dx%d\n", 'A' - 1 + ((id >> 10) & 0x1f), 'A' - 1 + ((id >> 5) & 0x1f),
                      'A' - 1 + (id & 0x1f), e[10] | e[11] << 8, w, h);
            for (uint32_t r = 0; r < p->edid_len; r += 16) { nv_sb_printf(&b, "           %03x: ", r); hexline(&b, e + r, 16); nv_sb_printf(&b, "\n"); }
        } else {
            nv_sb_printf(&b, "\n");
        }
    }

    if (d->state_supported && d->display_present) {
        nv_sb_printf(&b, "\nDisplay engine as the firmware left it (armed core channel state)\n");
        nv_sb_printf(&b, "  heads mask %x (%d heads), SOR mask %02x\n", d->head_mask, d->head_count, d->sor_mask);
        for (int i = 0; i < NV_MAX_SORS; i++)
            if (d->sor_mask & (1u << i))
                nv_sb_printf(&b, "  SOR %d: control %08x -> heads %x, %s\n", i, d->sor[i].ctrl, d->sor[i].heads,
                          d->sor[i].heads ? sor_proto(d->sor[i].proto) : "idle");
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(d->head_mask & (1u << h))) continue;
            const struct nv_head_state *s = &d->head[h];
            nv_sb_printf(&b, "  head %d: %s, raster %ux%u, sync end %u,%u, blank %u-%u x %u-%u, pixel clock %u kHz, %d bpp\n",
                      h, s->active ? "ACTIVE" : "off", s->htotal, s->vtotal, s->hsync_end, s->vsync_end,
                      s->hblank_end, s->hblank_start, s->vblank_end, s->vblank_start, s->pixel_hz / 1000, s->depth);
            nv_sb_printf(&b, "          surface offset %08x (VRAM %lx), size %08x, storage %08x, params %08x, viewport in %08x out %08x\n",
                      s->offset, (unsigned long)s->offset << 8, s->size, s->storage, s->params, s->viewport_in, s->viewport_out);
        }
    }
    if (d->nregs) {
        nv_sb_printf(&b, "\nRegister snapshot (read-only, for the modeset stage)\n");
        for (int i = 0; i < d->nregs; i++)
            nv_sb_printf(&b, "  %06x = %08x  %s\n", d->regs[i].reg, d->regs[i].val, d->regs[i].what);
    }
    return b.n < cap ? b.n : cap;
}
