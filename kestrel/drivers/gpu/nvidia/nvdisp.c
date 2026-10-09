/* =============================================================================
 *  nvdisp.c -- NVIDIA stage 2: experimental Pascal (GP102-GP108) modeset
 *
 *  Takes the display engine over from the firmware and lights every
 *  DisplayPort monitor that is connected but dark: one head, one SOR and one
 *  pitch-linear X8R8G8B8 frame buffer in VRAM each. Heads the firmware lit
 *  are carried over unchanged (their armed state is replayed into the new
 *  core channel) unless opts->takeover.
 *
 *  Sequence (nouveau names in brackets):
 *    1. plan        DPCD caps per DP output [nvkm_dp_enable], mode from EDID,
 *                   free head + SOR [nvkm_outp_acquire]
 *    2. takeover    capability copies, VBIOS hand-off, instance memory with
 *                   RAMHT + ctxdmas, interrupt enables, SOR power
 *                   [gf119_disp_init, nvkm_disp_init]
 *    3. core        core channel, push buffer in VRAM [gp102_disp_core_init]
 *    4. adopt       firmware heads/SORs replayed, UPDATE
 *    5. light       base channel per new head [gp102_disp_dmac_init], base
 *                   surface + core head/SOR methods, interlocked UPDATE; the
 *                   supervisor (polled) runs IED scripts, routes SORs, sets
 *                   VPLLs, trains DP links [gf119_disp_super, nvkm_dp_train]
 *    6. verify      armed state and DP lane status read back
 *  Every step is logged; nv_plat->checkpoint() is called between steps.
 *
 *  Derived from nouveau (drivers/gpu/drm/nouveau), Copyright Red Hat Inc.,
 *  MIT licence: nvkm/engine/disp/{nv50,gf119,gm200,gp102,dp,outp}.c,
 *  nvkm/subdev/devinit/gf100.c, nvkm/core/ramht.c,
 *  nvkm/engine/dma/usergf119.c, dispnv50/{core507d,head907d,head917d,
 *  base907c}.c. Not yet run on hardware.
 * ============================================================================= */
#include "nvpriv.h"
#include "nvinit.h"

/* ======================================================================== */
/*  constants                                                                  */
/* ======================================================================== */
#define EVO_PUSH_BYTES          0x1000u
#define EVO_PUSH_MAX_DW         (EVO_PUSH_BYTES / 4u - 1u - 12u)  /* last 12 dwords unusable pre-GV100 */

#define DISP_INST_SIZE          0x10000u            /* 64 KiB, 64 KiB aligned           */
#define DISP_RAMHT_SIZE         0x1000u             /* 512 entries x 8 bytes             */
#define DISP_RAMHT_ENTRIES      (DISP_RAMHT_SIZE / 8u)
#define DISP_RAMHT_BITS         9u
#define DISP_OBJ_BASE           0x1000u             /* DMA objects follow the RAMHT      */
#define DISP_OBJ_STRIDE         0x20u               /* 24 bytes, 16-byte aligned         */

/* our VRAM area, relative to ms.base */
#define AREA_INST               0x00000u
#define AREA_CORE_PUSH          0x10000u
#define AREA_BASE_PUSH(h)       (0x11000u + (uint32_t)(h) * 0x1000u)
#define AREA_SYNC               0x15000u
#define AREA_FB                 0x100000u           /* frame buffers from +1 MiB         */
#define AREA_MAX                (128ull << 20)
#define FB_ALIGN                (1ull << 20)

#define NV_HANDLE_SYNC          0xf0000000u         /* NV50_DISP_HANDLE_SYNCBUF          */
#define NV_HANDLE_VRAM          0xf0000001u         /* NV50_DISP_HANDLE_VRAM             */
#define NV_HANDLE_FB_PITCH      0xfb000000u         /* NV50_DISP_HANDLE_WNDW_CTX(kind 0) */

#define DMA_KIND_PITCH          0x00u
#define DMA_PAGE_LP             0x00u
#define DMA_PAGE_SP             0x01u
#define DMA_TARGET_VRAM         0x00000009u

#define EVO_CORE_CTRL           0
#define EVO_CORE_USER           0
#define EVO_BASE_CTRL(h)        (1 + (h))
#define EVO_BASE_USER(h)        (1 + (h))

/* core channel methods (907d / 917d) */
#define NV907D_UPDATE                               0x0080u
#define   NV907D_UPDATE_INTERLOCK_WITH_BASE(h)      (0x00000002u << ((h) * 8))
#define NV907D_SET_NOTIFIER_CONTROL                 0x0084u
#define   NV907D_SET_NOTIFIER_CONTROL_NOTIFY_ENABLE 0x80000000u
#define NV907D_SET_CONTEXT_DMA_NOTIFIER             0x0088u
#define NV907D_SOR_SET_CONTROL(o)                   (0x0200u + (uint32_t)(o) * 0x20u)
#define   NV907D_SOR_PROTOCOL_DP_A                  0x8u
#define   NV907D_SOR_PROTOCOL_DP_B                  0x9u

#define NV907D_HEAD(h, m)                           ((m) + (uint32_t)(h) * 0x300u)
#define NV907D_HEAD_SET_CONTROL_OUTPUT_RESOURCE     0x0404u
#define NV907D_HEAD_SET_OVERSCAN_COLOR              0x0410u
#define NV907D_HEAD_SET_DEFAULT_BASE_COLOR          0x042cu
#define NV907D_HEAD_SET_CONTEXT_DMA_CRC             0x0438u
#define NV907D_HEAD_SET_OUTPUT_LUT_LO               0x0448u
#define NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY       0x0450u
#define   NV907D_PIXEL_CLOCK_MODE_CLK_CUSTOM        (0x2u << 20)
#define NV907D_HEAD_SET_CONTEXT_DMA_LUT             0x045cu
#define NV907D_HEAD_SET_OFFSET                      0x0460u
#define NV907D_HEAD_SET_SIZE                        0x0468u
#define   NV907D_STORAGE_LAYOUT_PITCH               (1u << 24)
#define NV907D_HEAD_SET_CONTEXT_DMAS_ISO            0x0474u
#define NV917D_HEAD_SET_CONTROL_CURSOR              0x0480u
#define   NV917D_CURSOR_DISABLED_A8R8G8B8_64        ((1u << 24) | (1u << 26))
#define NV917D_HEAD_SET_CONTEXT_DMA_CURSOR          0x048cu
#define NV907D_HEAD_SET_CONTROL_OUTPUT_SCALER       0x0494u
#define NV907D_HEAD_SET_PROCAMP                     0x0498u
#define NV917D_HEAD_SET_DITHER_CONTROL              0x04a0u
#define NV907D_HEAD_SET_VIEWPORT_POINT_IN           0x04b0u
#define NV907D_HEAD_SET_VIEWPORT_SIZE_IN            0x04b8u
#define NV907D_HEAD_SET_VIEWPORT_SIZE_OUT           0x04c0u
#define NV917D_HEAD_SET_BASE_CHANNEL_USAGE_BOUNDS   0x04d0u
#define   NV917D_BASE_BOUNDS_USABLE_BPP32           ((0x3u << 8) | 0x1u)
#define NV917D_HEAD_SET_OVERLAY_USAGE_BOUNDS        0x04d4u
#define   NV917D_OVLY_BOUNDS_BPP16_UNUSED           (0x1u << 8)

#define NV_FORMAT_A8R8G8B8                          0xcfu
#define NV_PIXEL_DEPTH_BPP_24_444                   0x5u

/* base channel methods (907c) */
#define NV907C_UPDATE                               0x0080u
#define   NV907C_UPDATE_INTERLOCK_WITH_CORE         0x00000001u
#define NV907C_SET_PRESENT_CONTROL                  0x0084u
#define   NV907C_PRESENT_NON_TEARING_INTERVAL_1     (0x1u << 4)
#define NV907C_SET_CONTEXT_DMA_SEMAPHORE            0x0094u
#define NV907C_SET_CONTEXT_DMA_NOTIFIER             0x00a4u
#define NV907C_SET_CONTEXT_DMAS_ISO(b)              (0x00c0u + (b) * 4u)
#define NV907C_SET_BASE_LUT_LO                      0x00e0u
#define NV907C_SET_OUTPUT_LUT_LO                    0x00e8u
#define NV907C_SET_CONTEXT_DMA_LUT                  0x00fcu
#define NV907C_SET_CSC_RED2RED                      0x0140u
#define NV907C_SURFACE_SET_OFFSET(a, b)             (0x0400u + (a) * 0x20u + (b) * 4u)

/* DPCD */
#define DPCD_REV                0x000
#define DPCD_MAX_LINK_RATE      0x001
#define DPCD_MAX_LANE_COUNT     0x002
#define   DPCD_ENHANCED_FRAME   0x80
#define   DPCD_TPS3             0x40
#define DPCD_MAX_DOWNSPREAD     0x003
#define   DPCD_TPS4             0x80
#define DPCD_AUX_RD_INTERVAL    0x00e
#define DPCD_LINK_BW_SET        0x100
#define DPCD_TRAINING_PATTERN   0x102
#define DPCD_LANE0_SET          0x103
#define DPCD_LANE0_1_PC2_SET    0x10f
#define DPCD_LANE0_1_STATUS     0x202
#define DPCD_ADJUST_REQUEST     0x206
#define DPCD_ADJUST_PC2         0x20c
#define DPCD_SET_POWER          0x600
#define DPCD_LTTPR_REV          0xf0000
#define DPCD_LTTPR_MODE         0xf0003

/* ======================================================================== */
/*  state                                                                      */
/* ======================================================================== */
struct nv_evo_chan {
    const char *name;
    int      ctrl, user;
    bool     core;
    uint64_t push;
    uint32_t put;
    bool     overflow, up;
};

struct nv_surface {
    uint64_t addr;
    uint16_t width, height;
    uint32_t pitch;
    uint8_t  format;
    uint32_t handle;
};

struct nv_raster {
    uint16_t h_active, h_synce, h_blanke, h_blanks;
    uint16_t v_active, v_synce, v_blanke, v_blanks;
    uint16_t v_blank2s, v_blank2e;
    uint32_t hz;
    bool     nhsync, nvsync;
};

struct head_st {                    /* gf119_head_state */
    uint16_t htotal, vtotal, hsynce, vsynce, hblanke, vblanke, hblanks, vblanks;
    uint32_t hz;
    int      depth;
};

struct ior_st {                     /* gf119_sor_state */
    uint8_t  proto_evo;
    uint8_t  heads;
    int      link;
    bool     dp;
};

struct disp {
    struct nv_device *d;
    struct nv_modeset_opts opts;
    uint32_t obj_next;
    struct nv_evo_chan core, base[NV_MAX_HEADS];
    struct head_st head_arm[NV_MAX_HEADS], head_asy[NV_MAX_HEADS];
    struct ior_st  sor_armst[NV_MAX_SORS], sor_asyst[NV_MAX_SORS];
    bool     state_valid;
};

static struct disp X;
static struct nvinit_ops script_ops;

static void mzero(void *p, size_t n) { uint8_t *b = p; while (n--) *b++ = 0; }

static int ffs32(uint32_t v)
{
    for (int i = 0; i < 32; i++) if (v & (1u << i)) return i + 1;
    return 0;
}

static void checkpoint(struct nv_device *d, const char *stage)
{
    d->ms.stage = stage;
    nv_log("nvdisp: --- %s ---\n", stage);
    if (nv_plat->checkpoint) nv_plat->checkpoint(stage);
}

/* ======================================================================== */
/*  VRAM through the PRAMIN window                                             */
/* ======================================================================== */
static uint64_t pramin_window = ~0ull;

static void pramin_invalidate(void) { pramin_window = ~0ull; }

static uint32_t pramin_reg(struct nv_device *d, uint64_t addr)
{
    uint64_t win = addr & ~0xfffffull;
    if (win != pramin_window) {
        nv_wr32(d, 0x001700, (uint32_t)(win >> 16));
        pramin_window = win;
    }
    return 0x700000u + (uint32_t)(addr & 0xfffffu);
}

uint32_t nv_vram_rd32(struct nv_device *d, uint64_t addr) { return nv_rd32(d, pramin_reg(d, addr)); }
void nv_vram_wr32(struct nv_device *d, uint64_t addr, uint32_t v) { nv_wr32(d, pramin_reg(d, addr), v); }

void nv_vram_write(struct nv_device *d, uint64_t addr, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4)
        nv_vram_wr32(d, addr + i, (uint32_t)s[i] | (uint32_t)s[i + 1] << 8 |
                                  (uint32_t)s[i + 2] << 16 | (uint32_t)s[i + 3] << 24);
    if (i < len) {                                          /* partial last dword */
        uint32_t v = nv_vram_rd32(d, addr + i);
        for (uint32_t k = 0; i + k < len; k++)
            v = (v & ~(0xffu << (k * 8))) | (uint32_t)s[i + k] << (k * 8);
        nv_vram_wr32(d, addr + i, v);
    }
}

void nv_vram_fill(struct nv_device *d, uint64_t addr, uint32_t value, uint32_t len)
{
    for (uint32_t i = 0; i + 4 <= len; i += 4) nv_vram_wr32(d, addr + i, value);
}

/* ======================================================================== */
/*  VBIOS scripts                                                              */
/* ======================================================================== */
static int outp_pos(struct nv_device *d, const struct nvbios_output *o)
{
    if (!o) return -1;
    int pos = (int)(o - d->bios.output);
    return pos >= 0 && pos < d->bios.noutputs ? pos : -1;
}

static uint32_t sops_rd32(void *c, uint32_t r) { return nv_rd32(c, r); }
static void     sops_wr32(void *c, uint32_t r, uint32_t v) { nv_wr32(c, r, v); }
static uint8_t  sops_rd08(void *c, uint32_t r) { return nv_rd08(c, r); }
static void     sops_wr08(void *c, uint32_t r, uint8_t v) { nv_wr08(c, r, v); }
static void     sops_delay(void *c, uint32_t us) { (void)c; nv_udelay(us); }
static void     sops_log(void *c, const char *line) { (void)c; nv_log("%s\n", line); }

static int sops_aux_rd(void *c, const struct nvbios_output *o, uint32_t addr, uint8_t *v)
{
    struct nv_device *d = c;
    int pos = outp_pos(d, o);
    if (pos < 0 || d->dp[pos].aux < 0) return NV_ERR_NODEV;
    return nv_aux_dpcd_read(d, d->dp[pos].aux, addr, v, 1);
}

static int sops_aux_wr(void *c, const struct nvbios_output *o, uint32_t addr, uint8_t v)
{
    struct nv_device *d = c;
    int pos = outp_pos(d, o);
    if (pos < 0 || d->dp[pos].aux < 0) return NV_ERR_NODEV;
    return nv_aux_dpcd_write(d, d->dp[pos].aux, addr, &v, 1);
}

static int vpll_set(struct nv_device *d, int head, uint32_t khz);

static int sops_pll(void *c, uint32_t id, uint32_t khz)
{
    struct nv_device *d = c;
    if (id >= NVBIOS_PLL_VPLL0 && id < NVBIOS_PLL_VPLL0 + NV_MAX_HEADS)
        return vpll_set(d, (int)(id - NVBIOS_PLL_VPLL0), khz);
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (id == 0x614100u + (uint32_t)h * 0x800u) return vpll_set(d, h, khz);
    nv_log("nvdisp: script asks for PLL %08x = %u kHz, not supported\n", id, khz);
    return NV_ERR_NOSUPP;
}

static void script_ops_init(struct nv_device *d)
{
    script_ops = (struct nvinit_ops){
        .ctx = d, .rd32 = sops_rd32, .wr32 = sops_wr32, .rd08 = sops_rd08, .wr08 = sops_wr08,
        .delay_us = sops_delay, .aux_rd = sops_aux_rd, .aux_wr = sops_aux_wr,
        .pll_set = sops_pll, .log = sops_log,
    };
}

static void run_script(struct nv_device *d, uint32_t offset, int pos, int or, int link, int head, const char *what)
{
    if (!offset) return;
    struct nvinit st;
    const struct nvbios_output *o = pos >= 0 ? &d->bios.output[pos] : 0;
    int rc = nvinit_run(&d->bios, &script_ops, offset, o, or, link, head, X.opts.trace_scripts, &st);
    d->ms.script_runs++;
    d->ms.script_writes += st.writes;
    d->ms.script_warnings += st.warnings;
    nv_log("nvdisp: script %04x (%s) DCB %d SOR %d link %d head %d: %s, %u opcodes, %u writes, %u skipped, %u warnings\n",
           offset, what, pos, or, link, head, rc ? "FAILED" : "ok", st.opcodes, st.writes, st.skipped, st.warnings);
}

/* ======================================================================== */
/*  VPLL (gf100_devinit_pll_set)                                               */
/* ======================================================================== */
static uint32_t crystal_khz(struct nv_device *d)
{
    switch (nv_rd32(d, 0x101000) & 0x00400040u) {
    case 0x00000000: return 13500;
    case 0x00000040: return 14318;
    case 0x00400000: return 27000;
    default:         return 25000;
    }
}

static int vpll_set(struct nv_device *d, int head, uint32_t khz)
{
    struct nvbios_pll p;
    if (!nvbios_pll_parse(&d->bios, (uint8_t)(NVBIOS_PLL_VPLL0 + head), &p)) {
        nv_log("nvdisp: VPLL%d: no PLL limits entry (table version %02x)\n", head, p.ver);
        return NV_ERR_NOBIOS;
    }
    if (!p.reg) p.reg = 0x614100u + (uint32_t)head * 0x800u;
    if (!p.refclk) p.refclk = d->ms.crystal_khz;
    int N = 0, fN = 0, M = 0, P = 0;
    int got = nvbios_pll_calc(&p, khz, &N, &fN, &M, &P);
    if (got < 0 || !M) {
        nv_log("nvdisp: VPLL%d: no coefficients for %u kHz (ref %u, vco %u-%u, in %u-%u, M %u-%u, N %u-%u, P %u-%u)\n",
               head, khz, p.refclk, p.vco1.min_freq, p.vco1.max_freq, p.vco1.min_inputfreq, p.vco1.max_inputfreq,
               p.vco1.min_m, p.vco1.max_m, p.vco1.min_n, p.vco1.max_n, p.min_p, p.max_p);
        return NV_ERR_NOSUPP;
    }
    nv_mask(d, p.reg + 0x0c, 0x00000000, 0x00000100);
    nv_wr32(d, p.reg + 0x04, (uint32_t)P << 16 | (uint32_t)N << 8 | (uint32_t)M);
    nv_wr32(d, p.reg + 0x10, (uint32_t)fN << 16);
    nv_log("nvdisp: VPLL%d @%06x = %u kHz: ref %u, N %d fN %04x M %d P %d\n", head, p.reg, khz, p.refclk, N, fN, M, P);
    return NV_OK;
}

/* ======================================================================== */
/*  instance memory: RAMHT and DMA objects                                     */
/* ======================================================================== */
static uint32_t ramht_hash(int chid, uint32_t handle)
{
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1u << DISP_RAMHT_BITS) - 1u);
        handle >>= DISP_RAMHT_BITS;
    }
    hash ^= (uint32_t)chid << (DISP_RAMHT_BITS - 4u);
    return hash;
}

static int ctxdma_bind(struct nv_device *d, int chid, uint32_t handle, uint8_t kind, uint8_t page,
                       uint64_t start, uint64_t limit)
{
    if (chid < 0 || chid > 15 || X.obj_next + DISP_OBJ_STRIDE > DISP_INST_SIZE) return NV_ERR_NOMEM;
    const uint64_t inst = d->ms.inst;
    uint32_t off = X.obj_next;
    X.obj_next += DISP_OBJ_STRIDE;
    nv_vram_wr32(d, inst + off + 0x00, (uint32_t)kind << 20 | (uint32_t)page << 6 | DMA_TARGET_VRAM);
    nv_vram_wr32(d, inst + off + 0x04, (uint32_t)(start >> 8));
    nv_vram_wr32(d, inst + off + 0x08, (uint32_t)(limit >> 8));
    nv_vram_wr32(d, inst + off + 0x0c, 0);
    nv_vram_wr32(d, inst + off + 0x10, 0);
    nv_vram_wr32(d, inst + off + 0x14, 0);

    uint32_t co = ramht_hash(chid, handle) % DISP_RAMHT_ENTRIES, ho = co;
    do {
        uint64_t e = inst + (uint64_t)co * 8u;
        if (!nv_vram_rd32(d, e) && !nv_vram_rd32(d, e + 4)) {
            nv_vram_wr32(d, e + 0, handle);
            nv_vram_wr32(d, e + 4, (uint32_t)chid << 27 | 0x00000001u | off << 9);
            nv_log("nvdisp: RAMHT[%u] chid %d handle %08x -> object %04x [%lx-%lx] kind %x page %u\n",
                   co, chid, handle, off, (unsigned long)start, (unsigned long)limit, kind, page);
            return NV_OK;
        }
        if (++co >= DISP_RAMHT_ENTRIES) co = 0;
    } while (co != ho);
    return NV_ERR_NOMEM;
}

static uint64_t vram_limit(struct nv_device *d) { return d->vram_bytes - 1; }

static int bind_channel_objects(struct nv_device *d, int chid, bool core)
{
    int rc;
    if ((rc = ctxdma_bind(d, chid, NV_HANDLE_SYNC, DMA_KIND_PITCH, DMA_PAGE_SP, d->ms.sync, d->ms.sync + 0xfff))) return rc;
    if (core && (rc = ctxdma_bind(d, chid, NV_HANDLE_VRAM, DMA_KIND_PITCH, DMA_PAGE_SP, 0, vram_limit(d)))) return rc;
    return ctxdma_bind(d, chid, NV_HANDLE_FB_PITCH, DMA_KIND_PITCH, DMA_PAGE_LP, 0, vram_limit(d));
}

/* ======================================================================== */
/*  EVO channels                                                               */
/* ======================================================================== */
static uint32_t chan_ctrl_reg(const struct nv_evo_chan *c) { return 0x610490u + (uint32_t)c->ctrl * 0x10u; }
static uint32_t chan_put_reg(const struct nv_evo_chan *c)  { return 0x640000u + (uint32_t)c->ctrl * 0x1000u; }
static uint32_t chan_get_reg(const struct nv_evo_chan *c)  { return chan_put_reg(c) + 4u; }

static void evo_chan_fini(struct nv_device *d, struct nv_evo_chan *c)
{
    const uint32_t r = chan_ctrl_reg(c);
    if (c->core) nv_mask(d, r, 0x00000010, 0x00000000);
    else         nv_mask(d, r, 0x00001010, 0x00001000);
    nv_mask(d, r, 0x00000003, 0x00000000);
    if (!nv_wait(d, r, 0x001e0000, 0, 2000000))
        nv_log("nvdisp: %s channel fini timeout, ctrl %08x\n", c->name, nv_rd32(d, r));
    c->up = false;
}

static int evo_chan_init(struct nv_device *d, struct nv_evo_chan *c)
{
    const uint32_t r = chan_ctrl_reg(c), o = (uint32_t)c->ctrl * 0x10u;
    uint32_t was = nv_rd32(d, r);
    if (was & 0x00000013) {
        nv_log("nvdisp: %s channel active before init (ctrl %08x), stopping it\n", c->name, was);
        evo_chan_fini(d, c);
    }
    nv_vram_fill(d, c->push, 0, EVO_PUSH_BYTES);
    nv_wr32(d, 0x611494 + o, (uint32_t)(c->push >> 8) | 0x00000001u);
    nv_wr32(d, 0x611498 + o, 0x00010000);
    nv_wr32(d, 0x61149c + o, 0x00000001);
    nv_mask(d, r, 0x00000010, 0x00000010);
    nv_wr32(d, chan_put_reg(c), 0);
    nv_wr32(d, r, c->core ? 0x01000013u : 0x00000013u);
    if (!nv_wait(d, r, 0x80000000, 0, 2000000)) {
        nv_log("nvdisp: %s channel init timeout, ctrl %08x\n", c->name, nv_rd32(d, r));
        return NV_ERR_TIMEOUT;
    }
    c->put = 0;
    c->overflow = false;
    c->up = true;
    nv_log("nvdisp: %s channel up (ctrl %d), push buffer %lx, ctrl %08x\n", c->name, c->ctrl,
           (unsigned long)c->push, nv_rd32(d, r));
    return NV_OK;
}

static void evo_data(struct nv_device *d, struct nv_evo_chan *c, uint32_t v)
{
    if (c->put / 4u >= EVO_PUSH_MAX_DW) { c->overflow = true; return; }
    nv_vram_wr32(d, c->push + c->put, v);
    c->put += 4;
}

static void evo_mthd(struct nv_device *d, struct nv_evo_chan *c, uint32_t mthd, uint32_t count)
{
    evo_data(d, c, count << 18 | mthd);
}

static void evo_mthd1(struct nv_device *d, struct nv_evo_chan *c, uint32_t mthd, uint32_t v)
{
    evo_mthd(d, c, mthd, 1);
    evo_data(d, c, v);
}

static int evo_kick(struct nv_device *d, struct nv_evo_chan *c)
{
    if (c->overflow) { nv_log("nvdisp: %s push buffer overflow\n", c->name); return NV_ERR_NOMEM; }
    nv_wr32(d, 0x070000, 0x00000001);
    if (!nv_wait(d, 0x070000, 0x00000002, 0, 2000000)) nv_log("nvdisp: 0x070000 flush timeout\n");
    nv_wr32(d, chan_put_reg(c), c->put);
    return NV_OK;
}

static bool evo_idle(struct nv_device *d, struct nv_evo_chan *c)
{
    return nv_rd32(d, chan_get_reg(c)) == c->put;
}

static void chan_errors(struct nv_device *d)
{
    uint32_t stat = nv_rd32(d, 0x61009c);
    for (int chid = 0; stat && chid < 32; chid++) {
        if (!(stat & (1u << chid))) continue;
        uint32_t mthd = nv_rd32(d, 0x6111f0 + (uint32_t)chid * 12u);
        uint32_t data = nv_rd32(d, 0x6111f4 + (uint32_t)chid * 12u);
        uint32_t unkn = nv_rd32(d, 0x6111f8 + (uint32_t)chid * 12u);
        nv_log("nvdisp: channel %d EXCEPTION: method %04x data %08x (%08x %08x)\n",
               chid, mthd & 0xffcu, data, mthd, unkn);
        nv_wr32(d, 0x61009c, 1u << chid);
        nv_wr32(d, 0x6111f0 + (uint32_t)chid * 12u, 0x90000000u);
        d->ms.chan_errors++;
    }
}

/* ======================================================================== */
/*  raster and surfaces                                                        */
/* ======================================================================== */
static void raster_from_mode(const struct nv_mode *m, struct nv_raster *r)
{
    r->h_active  = m->htotal;
    r->h_synce   = (uint16_t)(m->hsync_end - m->hsync_start - 1);
    r->h_blanke  = (uint16_t)(m->htotal - m->hsync_start - 1);
    r->h_blanks  = (uint16_t)(r->h_blanke + m->hdisplay);
    r->v_active  = m->vtotal;
    r->v_synce   = (uint16_t)(m->vsync_end - m->vsync_start - 1);
    r->v_blanke  = (uint16_t)(m->vtotal - m->vsync_start - 1);
    r->v_blanks  = (uint16_t)(r->v_blanke + m->vdisplay);
    r->v_blank2s = 1;
    r->v_blank2e = 0;
    r->hz        = m->clock_khz * 1000u;
    r->nhsync    = m->nhsync;
    r->nvsync    = m->nvsync;
}

static uint32_t surface_pitch(uint16_t w) { return ((uint32_t)w + 63u) / 64u * 64u * 4u; }

static void surface_of(const struct nv_output *o, struct nv_surface *s)
{
    s->addr   = o->fb;
    s->width  = o->mode.hdisplay;
    s->height = o->mode.vdisplay;
    s->pitch  = o->pitch;
    s->format = NV_FORMAT_A8R8G8B8;
    s->handle = NV_HANDLE_FB_PITCH;
}

/* ======================================================================== */
/*  core channel: head and SOR methods                                         */
/* ======================================================================== */
static void core_head_program(struct nv_device *d, struct nv_evo_chan *c, const struct nv_output *o)
{
    const int i = o->head;
    struct nv_raster r;
    struct nv_surface s;
    raster_from_mode(&o->mode, &r);
    surface_of(o, &s);
    const uint32_t wh = (uint32_t)s.height << 16 | s.width;

    /* head907d_or */
    evo_mthd(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_CONTROL_OUTPUT_RESOURCE), 2);
    evo_data(d, c, (uint32_t)r.nhsync << 3 | (uint32_t)r.nvsync << 4 | NV_PIXEL_DEPTH_BPP_24_444 << 6);
    evo_data(d, c, 0x31ec6000u | (uint32_t)i << 25);
    /* procamp, dither */
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_PROCAMP), 0x400u << 8);
    evo_mthd1(d, c, NV907D_HEAD(i, NV917D_HEAD_SET_DITHER_CONTROL), 0);
    /* primary plane routed to the base channel */
    evo_mthd1(d, c, NV907D_HEAD(i, NV917D_HEAD_SET_OVERLAY_USAGE_BOUNDS), NV917D_OVLY_BOUNDS_BPP16_UNUSED);
    evo_mthd1(d, c, NV907D_HEAD(i, NV917D_HEAD_SET_BASE_CHANNEL_USAGE_BOUNDS), NV917D_BASE_BOUNDS_USABLE_BPP32);
    /* cursor, LUT off */
    evo_mthd1(d, c, NV907D_HEAD(i, NV917D_HEAD_SET_CONTROL_CURSOR), NV917D_CURSOR_DISABLED_A8R8G8B8_64);
    evo_mthd1(d, c, NV907D_HEAD(i, NV917D_HEAD_SET_CONTEXT_DMA_CURSOR), 0);
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_OUTPUT_LUT_LO), 0);
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_CONTEXT_DMA_LUT), 0);
    /* core surface = the base surface (head907d_core_set) */
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_OFFSET), (uint32_t)(s.addr >> 8));
    evo_mthd(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_SIZE), 4);
    evo_data(d, c, wh);
    evo_data(d, c, (s.pitch >> 8) << 8 | NV907D_STORAGE_LAYOUT_PITCH);
    evo_data(d, c, (uint32_t)s.format << 8);
    evo_data(d, c, s.handle);
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_VIEWPORT_POINT_IN), 0);
    /* head907d_mode */
    evo_mthd(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_OVERSCAN_COLOR), 6);
    evo_data(d, c, 0);
    evo_data(d, c, (uint32_t)r.v_active  << 16 | r.h_active);
    evo_data(d, c, (uint32_t)r.v_synce   << 16 | r.h_synce);
    evo_data(d, c, (uint32_t)r.v_blanke  << 16 | r.h_blanke);
    evo_data(d, c, (uint32_t)r.v_blanks  << 16 | r.h_blanks);
    evo_data(d, c, (uint32_t)r.v_blank2e << 16 | r.v_blank2s);
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_DEFAULT_BASE_COLOR), 0);
    evo_mthd(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY), 3);
    evo_data(d, c, r.hz);
    evo_data(d, c, NV907D_PIXEL_CLOCK_MODE_CLK_CUSTOM);
    evo_data(d, c, r.hz);
    /* head907d_view */
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_CONTROL_OUTPUT_SCALER), 0);
    evo_mthd1(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_VIEWPORT_SIZE_IN), wh);
    evo_mthd(d, c, NV907D_HEAD(i, NV907D_HEAD_SET_VIEWPORT_SIZE_OUT), 3);
    evo_data(d, c, wh);
    evo_data(d, c, wh);
    evo_data(d, c, wh);
    /* SOR */
    evo_mthd1(d, c, NV907D_SOR_SET_CONTROL(o->sor),
              (o->link == 2 ? NV907D_SOR_PROTOCOL_DP_B : NV907D_SOR_PROTOCOL_DP_A) << 8 | 1u << i);
}

/* Head methods nouveau dumps for GK104+ core channels (gk104.c). */
static const uint16_t head_mthds[] = {
    0x400, 0x404, 0x408, 0x40c, 0x410, 0x414, 0x418, 0x41c, 0x420, 0x424, 0x428, 0x42c, 0x430, 0x434,
    0x438, 0x440, 0x444, 0x448, 0x44c, 0x450, 0x454, 0x458, 0x45c, 0x460, 0x468, 0x46c, 0x470, 0x474,
    0x47c, 0x480, 0x484, 0x488, 0x48c, 0x490, 0x494, 0x498, 0x4a0, 0x4b0, 0x4b8, 0x4bc, 0x4c0, 0x4c4,
    0x4c8, 0x4d0, 0x4d4, 0x4e0, 0x4e4,
};
static const uint16_t sor_mthds[] = { 0x200, 0x204, 0x208, 0x210 };

/* Replay the armed state of a firmware head, swapping the firmware's ctxdma
 * handles (they live in its RAMHT, not ours) for ours or for none. */
static void core_head_adopt(struct nv_device *d, struct nv_evo_chan *c, int head)
{
    int diffs = 0;
    for (size_t k = 0; k < sizeof head_mthds / sizeof *head_mthds; k++) {
        const uint32_t m = head_mthds[k];
        uint32_t arm = nv_rd32(d, 0x640000u + NV907D_HEAD(head, m));
        uint32_t asy = nv_rd32(d, 0x660000u + NV907D_HEAD(head, m));
        uint32_t v = arm;
        switch (m) {
        case NV907D_HEAD_SET_CONTEXT_DMA_CRC:
        case NV907D_HEAD_SET_CONTEXT_DMA_LUT:
        case NV917D_HEAD_SET_CONTEXT_DMA_CURSOR: v = 0; break;
        case NV907D_HEAD_SET_OUTPUT_LUT_LO:
        case NV917D_HEAD_SET_CONTROL_CURSOR:     v = arm & ~0x80000000u; break;
        case NV907D_HEAD_SET_CONTEXT_DMAS_ISO:   v = NV_HANDLE_FB_PITCH; break;
        default:
            if ((arm & 0xf0000000u) == 0xf0000000u && m != 0x408 && m != 0x480) {
                nv_log("nvdisp: head %d method %03x armed %08x looks like a handle, sending 0\n", head, m, arm);
                v = 0;
            }
            break;
        }
        if (arm != asy) {
            diffs++;
            nv_log("nvdisp: head %d method %03x: armed %08x, assembly %08x\n", head, m, arm, asy);
        }
        evo_mthd1(d, c, NV907D_HEAD(head, m), v);
    }
    nv_log("nvdisp: head %d adopted (%d method(s) differed between armed and assembly state)\n", head, diffs);
}

static void core_sor_adopt(struct nv_device *d, struct nv_evo_chan *c, int sor)
{
    for (size_t k = 0; k < sizeof sor_mthds / sizeof *sor_mthds; k++) {
        const uint32_t m = sor_mthds[k] + (uint32_t)sor * 0x20u;
        uint32_t arm = nv_rd32(d, 0x640000u + m), asy = nv_rd32(d, 0x660000u + m);
        if (arm != asy) nv_log("nvdisp: SOR %d method %03x: armed %08x, assembly %08x\n", sor, m, arm, asy);
        evo_mthd1(d, c, m, arm);
    }
}

/* ======================================================================== */
/*  base channel: the primary plane                                            */
/* ======================================================================== */
static void base_clear(struct nv_device *d, struct nv_evo_chan *b)
{
    static const uint32_t csc_identity[12] = {
        0x00010000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00010000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00010000, 0x00000000,
    };
    evo_mthd1(d, b, NV907C_SET_CONTEXT_DMA_NOTIFIER, 0);
    evo_mthd1(d, b, NV907C_SET_CONTEXT_DMA_SEMAPHORE, 0);
    evo_mthd1(d, b, NV907C_SET_BASE_LUT_LO, 0);
    evo_mthd1(d, b, NV907C_SET_OUTPUT_LUT_LO, 0);
    evo_mthd1(d, b, NV907C_SET_CONTEXT_DMA_LUT, 0);
    evo_mthd(d, b, NV907C_SET_CSC_RED2RED, 12);
    for (int k = 0; k < 12; k++) evo_data(d, b, csc_identity[k]);
}

static void base_image(struct nv_device *d, struct nv_evo_chan *b, const struct nv_output *o)
{
    struct nv_surface s;
    surface_of(o, &s);
    evo_mthd1(d, b, NV907C_SET_PRESENT_CONTROL, NV907C_PRESENT_NON_TEARING_INTERVAL_1);
    evo_mthd1(d, b, NV907C_SET_CONTEXT_DMAS_ISO(0), s.handle);
    evo_mthd(d, b, NV907C_SURFACE_SET_OFFSET(0, 0), 5);
    evo_data(d, b, (uint32_t)(s.addr >> 8));
    evo_data(d, b, 0);
    evo_data(d, b, (uint32_t)s.height << 16 | s.width);
    evo_data(d, b, (s.pitch >> 8) << 8 | NV907D_STORAGE_LAYOUT_PITCH);
    evo_data(d, b, (uint32_t)s.format << 8);
    evo_mthd1(d, b, NV907C_UPDATE, NV907C_UPDATE_INTERLOCK_WITH_CORE);
}

static int base_create(struct nv_device *d, int head)
{
    struct nv_evo_chan *b = &X.base[head];
    b->name = "base";
    b->ctrl = EVO_BASE_CTRL(head);
    b->user = EVO_BASE_USER(head);
    b->core = false;
    b->push = d->ms.base + AREA_BASE_PUSH(head);
    int rc = bind_channel_objects(d, b->user, false);
    if (!rc) rc = evo_chan_init(d, b);
    if (rc) return rc;
    base_clear(d, b);
    rc = evo_kick(d, b);
    for (int t = 0; !rc && !evo_idle(d, b); t++) {
        if (t > 200000) { nv_log("nvdisp: base %d channel did not consume its init methods\n", head); return NV_ERR_TIMEOUT; }
        chan_errors(d);
        nv_udelay(10);
    }
    return rc;
}

/* ======================================================================== */
/*  DisplayPort (nvkm/engine/disp/dp.c, gf119/gm107/gm200 SOR DP)              */
/* ======================================================================== */
static uint32_t sor_soff(int sor) { return (uint32_t)sor * 0x800u; }
static uint32_t sor_loff(int sor, int link) { return sor_soff(sor) + (link == 2 ? 0x80u : 0u); }

static int dp_rd(struct nv_device *d, int pos, uint32_t addr, uint8_t *buf, uint32_t n)
{
    return nv_aux_dpcd_read(d, d->dp[pos].aux, addr, buf, n);
}

static int dp_wr(struct nv_device *d, int pos, uint32_t addr, const uint8_t *buf, uint32_t n)
{
    return nv_aux_dpcd_write(d, d->dp[pos].aux, addr, buf, n);
}

/* nvkm_dp_enable: LTTPRs, receiver caps, usable lanes and rates. */
static bool dp_enable(struct nv_device *d, int pos)
{
    const struct nvbios_output *o = &d->bios.output[pos];
    struct nv_dp_probe *p = &d->dp[pos];
    typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    mzero(s, sizeof *s);
    if (p->aux < 0 || !p->sink || p->dpcd_rc != NV_OK) return false;

    if (dp_rd(d, pos, DPCD_LTTPR_REV, s->lttpr, sizeof s->lttpr) != NV_OK) mzero(s->lttpr, sizeof s->lttpr);
    if (s->lttpr[0] >= 0x14 && s->lttpr[2])
        nv_log("nvdisp: DCB %d: %02x LTTPR(s) present, training them in transparent mode only\n", pos, s->lttpr[2]);

    if (p->dpcd[DPCD_AUX_RD_INTERVAL] & 0x80) {             /* extended receiver caps at 0x2200 */
        uint8_t ext[16];
        if (dp_rd(d, pos, 0x2200, ext, sizeof ext) == NV_OK && ext[0] >= p->dpcd[0]) {
            for (int i = 0; i < 16; i++) p->dpcd[i] = ext[i];
            nv_log("nvdisp: DCB %d: using extended DPCD caps (rev %02x)\n", pos, ext[0]);
        }
    }
    s->links = p->dpcd[DPCD_MAX_LANE_COUNT] & 0x1f;
    if (s->links > o->dp_link_nr) s->links = o->dp_link_nr;
    uint8_t rate_max = p->dpcd[DPCD_MAX_LINK_RATE];
    if (rate_max > o->dp_link_bw) rate_max = o->dp_link_bw;
    static const uint8_t rates[] = { 0x1e, 0x14, 0x0a, 0x06 };
    for (int i = 0; i < 4; i++) if (rates[i] <= rate_max) s->rate[s->rates++] = rates[i];
    nv_log("nvdisp: DCB %d: DPCD %02x, %u lane(s), %u rate(s), max %u x %u kB/s\n", pos, p->dpcd[0], s->links,
           s->rates, s->links, s->rates ? s->rate[0] * 27000u : 0);
    return s->links && s->rates;
}

struct lt_state {
    struct nv_device *d;
    int      pos, sor, link, nr;
    struct nvbios_dpout info;
    bool     have_info;
    uint8_t  stat[6], conf[4], pc2stat, pc2conf[2];
    bool     pc2;
};

static void sor_dp_drive(struct nv_device *d, int sor, int link, int ln, int pc, int dc, int pe, int pu)
{
    const uint32_t loff = sor_loff(sor, link), shift = (uint32_t)ln * 8u;   /* gm200: lanes {0,1,2,3} */
    uint32_t data[4];
    pu &= 0x0f;
    data[0] = nv_rd32(d, 0x61c118 + loff) & ~(0xffu << shift);
    data[1] = nv_rd32(d, 0x61c120 + loff) & ~(0xffu << shift);
    data[2] = nv_rd32(d, 0x61c130 + loff);
    if ((data[2] & 0x00000f00u) < ((uint32_t)pu << 8) || ln == 0)
        data[2] = (data[2] & ~0x00000f00u) | (uint32_t)pu << 8;
    nv_wr32(d, 0x61c118 + loff, data[0] | (uint32_t)dc << shift);
    nv_wr32(d, 0x61c120 + loff, data[1] | (uint32_t)pe << shift);
    nv_wr32(d, 0x61c130 + loff, data[2]);
    data[3] = nv_rd32(d, 0x61c13c + loff) & ~(0xffu << shift);
    nv_wr32(d, 0x61c13c + loff, data[3] | (uint32_t)pc << shift);
}

static void sor_dp_pattern(struct nv_device *d, int sor, int link, int pattern)
{
    static const uint32_t data[] = { 0x10101010, 0x01010101, 0x02020202, 0x03030303, 0x1b1b1b1b };
    const uint32_t soff = sor_soff(sor);
    if (pattern < 0 || pattern > 4) return;
    if (link & 1) nv_mask(d, 0x61c110 + soff, 0x1f1f1f1f, data[pattern]);
    else          nv_mask(d, 0x61c12c + soff, 0x1f1f1f1f, data[pattern]);
}

static void sor_dp_links(struct nv_device *d, int sor, int link, uint8_t bw, int nr, bool ef)
{
    uint32_t dpctrl = ((1u << nr) - 1u) << 16, clksor = (uint32_t)bw << 18;
    if (ef) dpctrl |= 0x00004000;
    nv_mask(d, 0x612300 + sor_soff(sor), 0x007c0000, clksor);
    nv_mask(d, 0x61c10c + sor_loff(sor, link), 0x401f4000, dpctrl);
}

static void sor_dp_power(struct nv_device *d, int sor, int link, int nr)
{
    uint32_t mask = 0;
    for (int i = 0; i < nr; i++) mask |= 1u << i;
    nv_mask(d, 0x61c130 + sor_loff(sor, link), 0x0000000f, mask);
    nv_mask(d, 0x61c034 + sor_soff(sor), 0x80000000, 0x80000000);
    if (!nv_wait(d, 0x61c034 + sor_soff(sor), 0x80000000, 0, 2000000))
        nv_log("nvdisp: SOR %d DP power timeout\n", sor);
}

static int lt_sense(struct lt_state *lt, bool pc, uint32_t delay)
{
    nv_udelay(delay);
    int rc = dp_rd(lt->d, lt->pos, DPCD_LANE0_1_STATUS, &lt->stat[0], 3);
    if (rc) return rc;
    rc = dp_rd(lt->d, lt->pos, DPCD_ADJUST_REQUEST, &lt->stat[4], 2);
    if (rc) return rc;
    if (pc && dp_rd(lt->d, lt->pos, DPCD_ADJUST_PC2, &lt->pc2stat, 1)) lt->pc2stat = 0;
    nv_log("nvdisp:   status %02x %02x %02x adjust %02x %02x pc2 %02x\n", lt->stat[0], lt->stat[1], lt->stat[2],
           lt->stat[4], lt->stat[5], lt->pc2stat);
    return NV_OK;
}

static int lt_drive(struct lt_state *lt, bool pc)
{
    for (int i = 0; i < lt->nr; i++) {
        uint8_t lane = (uint8_t)((lt->stat[4 + (i >> 1)] >> ((i & 1) * 4)) & 0xf);
        uint8_t lpc2 = (uint8_t)((lt->pc2stat >> (i * 2)) & 0x3);
        uint8_t lpre = (lane & 0x0c) >> 2;
        uint8_t lvsw = lane & 0x03;
        uint8_t hivs = (uint8_t)(3 - lpre);
        if (lpc2 >= 3) lpc2 = 3 | 0x04;
        if (lpre >= 3) {
            lpre = 3 | 0x04;
            lvsw = hivs = (uint8_t)(3 - (lpre & 3));
        } else if (lvsw >= hivs) {
            lvsw = hivs | 0x04;
        }
        lt->conf[i] = (uint8_t)(lpre << 3 | lvsw);
        lt->pc2conf[i >> 1] |= (uint8_t)(lpc2 << ((i & 1) * 4));

        struct nvbios_dpcfg cfg;
        if (lt->have_info && nvbios_dpcfg_match(&lt->d->bios, &lt->info, lpc2 & 3, lvsw & 3, lpre & 3, &cfg))
            sor_dp_drive(lt->d, lt->sor, lt->link, i, cfg.pc, cfg.dc, cfg.pe, cfg.tx_pu);
        else
            nv_log("nvdisp:   lane %d: no DP drive config for vs %d pe %d pc %d\n", i, lvsw & 3, lpre & 3, lpc2 & 3);
    }
    int rc = dp_wr(lt->d, lt->pos, DPCD_LANE0_SET, lt->conf, 4);
    if (!rc && pc) rc = dp_wr(lt->d, lt->pos, DPCD_LANE0_1_PC2_SET, lt->pc2conf, 2);
    return rc;
}

static void lt_pattern(struct lt_state *lt, uint8_t pattern)
{
    uint8_t tp = 0;
    sor_dp_pattern(lt->d, lt->sor, lt->link, pattern);
    dp_rd(lt->d, lt->pos, DPCD_TRAINING_PATTERN, &tp, 1);
    tp &= (uint8_t)~0x0f;
    tp |= pattern != 4 ? pattern : 7;
    if (pattern) tp |= 0x20; else tp &= (uint8_t)~0x20;
    dp_wr(lt->d, lt->pos, DPCD_TRAINING_PATTERN, &tp, 1);
}

static int lt_cr(struct lt_state *lt)
{
    const uint8_t *dpcd = lt->d->dp[lt->pos].dpcd;
    bool cr_done = false, abort = false;
    int voltage = lt->conf[0] & 0x03, tries = 0;
    uint32_t usec = 0;
    lt_pattern(lt, 1);
    if (dpcd[DPCD_REV] < 0x14) usec = (dpcd[DPCD_AUX_RD_INTERVAL] & 0x7f) * 4000u;
    do {
        if (lt_drive(lt, false) || lt_sense(lt, false, usec ? usec : 100)) break;
        cr_done = true;
        for (int i = 0; i < lt->nr; i++) {
            uint8_t lane = (uint8_t)((lt->stat[i >> 1] >> ((i & 1) * 4)) & 0xf);
            if (!(lane & 0x01)) {
                cr_done = false;
                if (lt->conf[i] & 0x04) abort = true;
                break;
            }
        }
        if ((lt->conf[0] & 0x03) != voltage) { voltage = lt->conf[0] & 0x03; tries = 0; }
    } while (!cr_done && !abort && ++tries < 5);
    return cr_done ? 0 : -1;
}

static int lt_eq(struct lt_state *lt)
{
    const uint8_t *dpcd = lt->d->dp[lt->pos].dpcd;
    bool eq_done = false, cr_done = true;
    int tries = 0;
    if (dpcd[DPCD_REV] >= 0x14 && (dpcd[DPCD_MAX_DOWNSPREAD] & DPCD_TPS4)) lt_pattern(lt, 4);
    else if (dpcd[DPCD_REV] >= 0x12 && (dpcd[DPCD_MAX_LANE_COUNT] & DPCD_TPS3)) lt_pattern(lt, 3);
    else lt_pattern(lt, 2);
    uint32_t usec = (dpcd[DPCD_AUX_RD_INTERVAL] & 0x7f) * 4000u;
    do {
        if ((tries && lt_drive(lt, lt->pc2)) || lt_sense(lt, lt->pc2, usec ? usec : 400)) break;
        eq_done = lt->stat[2] & 0x01;
        for (int i = 0; i < lt->nr && eq_done; i++) {
            uint8_t lane = (uint8_t)((lt->stat[i >> 1] >> ((i & 1) * 4)) & 0xf);
            if (!(lane & 0x01)) cr_done = false;
            if (!(lane & 0x02) || !(lane & 0x04)) eq_done = false;
        }
    } while (!eq_done && cr_done && ++tries <= 5);
    return eq_done ? 0 : -1;
}

static int dp_train_link(struct nv_device *d, int pos, int sor, int link, const struct nvbios_dpout *info, bool have_info)
{
    typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    struct lt_state lt;
    mzero(&lt, sizeof lt);
    lt.d = d; lt.pos = pos; lt.sor = sor; lt.link = link; lt.nr = s->nr;
    lt.have_info = have_info;
    if (have_info) lt.info = *info;
    lt.pc2 = d->dp[pos].dpcd[DPCD_MAX_LANE_COUNT] & DPCD_TPS3;

    if (s->lttpr[0] >= 0x14) {
        uint8_t mode = 0x55;                                /* transparent */
        dp_wr(d, pos, DPCD_LTTPR_MODE, &mode, 1);
    }
    uint8_t sink[2] = { s->bw, (uint8_t)(s->nr | (s->ef ? 0x80 : 0)) };
    int rc = dp_wr(d, pos, DPCD_LINK_BW_SET, sink, 2);
    if (rc) return rc;
    rc = lt_cr(&lt);
    nv_log("nvdisp: DCB %d: clock recovery %s\n", pos, rc ? "FAILED" : "done");
    if (rc == 0) {
        rc = lt_eq(&lt);
        nv_log("nvdisp: DCB %d: channel equalisation %s\n", pos, rc ? "FAILED" : "done");
    }
    lt_pattern(&lt, 0);
    return rc;
}

static int dp_train_links(struct nv_device *d, int pos, int sor, int link, const struct nvbios_dpout *info, bool have_info)
{
    typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    nv_log("nvdisp: DCB %d: programming SOR %d link %d for %u lane(s) x %02x\n", pos, sor, link, s->nr, s->bw);
    if (have_info && info->lnkcmp) {
        uint32_t l = info->lnkcmp;
        for (int g = 0; g < 16 && s->bw < nvbios_rd08(&d->bios, l); g++) l += info->ver < 0x30 ? 4 : 3;
        run_script(d, nvbios_rd16(&d->bios, l + 1), pos, sor, link, -1, "DP link rate");
    }
    sor_dp_links(d, sor, link, s->bw, s->nr, s->ef);
    sor_dp_power(d, sor, link, s->nr);
    return dp_train_link(d, pos, sor, link, info, have_info);
}

static int dp_train(struct nv_device *d, int pos, int sor, int link, uint32_t dataKBps)
{
    typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    const uint8_t *dpcd = d->dp[pos].dpcd;
    const struct nvbios_output *o = &d->bios.output[pos];
    struct nvbios_dpout info;
    bool have_info = nvbios_dpout_match(&d->bios, o->hasht, o->hashm, &info) != 0;
    if (!have_info) nv_log("nvdisp: DCB %d: no DP table entry for %04x:%04x\n", pos, o->hasht, o->hashm);

    uint8_t pwr;
    if (dp_rd(d, pos, DPCD_SET_POWER, &pwr, 1) == NV_OK && (pwr & 0x03) != 0x01) {
        pwr = (uint8_t)((pwr & ~0x03) | 0x01);
        dp_wr(d, pos, DPCD_SET_POWER, &pwr, 1);
        nv_udelay(1000);
    }
    s->ef = dpcd[DPCD_MAX_LANE_COUNT] & DPCD_ENHANCED_FRAME;
    s->nr = 0;

    if (have_info) {
        run_script(d, (dpcd[DPCD_MAX_DOWNSPREAD] & 0x01) ? info.script[2] : info.script[3], pos, sor, link, -1,
                   (dpcd[DPCD_MAX_DOWNSPREAD] & 0x01) ? "DP EnableSpread" : "DP DisableSpread");
        run_script(d, info.script[0], pos, sor, link, -1, "DP BeforeLinkTraining");
    }
    int rc = -1;
    for (uint8_t nr = s->links; rc < 0 && nr; nr >>= 1) {
        for (int r = 0; rc < 0 && r < s->rates; r++) {
            if ((uint32_t)s->rate[r] * 27000u * nr >= dataKBps || !s->nr) {
                s->bw = s->rate[r];
                s->nr = nr;
                rc = dp_train_links(d, pos, sor, link, &info, have_info);
            }
        }
    }
    if (have_info) run_script(d, info.script[1], pos, sor, link, -1, "DP AfterLinkTraining");
    s->lt_done = true;
    nv_log("nvdisp: DCB %d: link training %s (%u lane(s) x %02x, %u kB/s needed)\n", pos, rc < 0 ? "FAILED" : "done",
           s->nr, s->bw, dataKBps);
    return rc;
}

static void dp_disable(struct nv_device *d, int pos, int sor, int link)
{
    const struct nvbios_output *o = &d->bios.output[pos];
    struct nvbios_dpout info;
    if (nvbios_dpout_match(&d->bios, o->hasht, o->hashm, &info))
        run_script(d, info.script[4], pos, sor, link, -1, "DP DisableLT");
}

/* ======================================================================== */
/*  display supervisor (gf119_disp_super, nv50_disp_super_*)                   */
/* ======================================================================== */
static void head_state(struct nv_device *d, int h, bool asy, struct head_st *s)
{
    const uint32_t hoff = (asy ? 0x20000u : 0u) + (uint32_t)h * 0x300u;
    uint32_t v;
    v = nv_rd32(d, 0x640414 + hoff); s->vtotal  = (uint16_t)(v >> 16); s->htotal  = (uint16_t)v;
    v = nv_rd32(d, 0x640418 + hoff); s->vsynce  = (uint16_t)(v >> 16); s->hsynce  = (uint16_t)v;
    v = nv_rd32(d, 0x64041c + hoff); s->vblanke = (uint16_t)(v >> 16); s->hblanke = (uint16_t)v;
    v = nv_rd32(d, 0x640420 + hoff); s->vblanks = (uint16_t)(v >> 16); s->hblanks = (uint16_t)v;
    s->hz = nv_rd32(d, 0x640450 + hoff);
    switch ((nv_rd32(d, 0x640404 + hoff) & 0x000003c0) >> 6) {
    case 6:  s->depth = 30; break;
    case 5:  s->depth = 24; break;
    default: s->depth = 18; break;
    }
}

static void ior_state(struct nv_device *d, int sor, bool asy, struct ior_st *s)
{
    uint32_t ctrl = nv_rd32(d, 0x640200 + (asy ? 0x20000u : 0u) + (uint32_t)sor * 0x20u);
    s->proto_evo = (uint8_t)((ctrl & 0x00000f00) >> 8);
    s->heads = (uint8_t)(ctrl & 0x0000000f);
    s->dp = false;
    switch (s->proto_evo) {
    case 0: case 1: s->link = 1; break;
    case 2:         s->link = 2; break;
    case 5:         s->link = 3; break;
    case 8:         s->link = 1; s->dp = true; break;
    case 9:         s->link = 2; s->dp = true; break;
    default:        s->link = 0; break;
    }
}

static void super_state(struct nv_device *d)
{
    for (int h = 0; h < NV_MAX_HEADS; h++) {
        if (!(d->head_mask & (1u << h))) continue;
        head_state(d, h, false, &X.head_arm[h]);
        head_state(d, h, true, &X.head_asy[h]);
    }
    for (int s = 0; s < NV_MAX_SORS; s++) {
        if (!(d->sor_mask & (1u << s))) continue;
        ior_state(d, s, false, &X.sor_armst[s]);
        ior_state(d, s, true, &X.sor_asyst[s]);
    }
    X.state_valid = true;
}

static int ior_arm(struct nv_device *d, int head)
{
    for (int s = 0; s < NV_MAX_SORS; s++)
        if ((d->sor_mask & (1u << s)) && (X.sor_armst[s].heads & (1u << head))) return s;
    return -1;
}

static int ior_asy(struct nv_device *d, int head)
{
    for (int s = 0; s < NV_MAX_SORS; s++)
        if ((d->sor_mask & (1u << s)) && (X.sor_asyst[s].heads & (1u << head))) return s;
    return -1;
}

static uint32_t iedt(struct nv_device *d, int head, int pos, struct nvbios_outp *t)
{
    const struct nvbios_output *o = &d->bios.output[pos];
    const uint16_t m = (uint16_t)((0x0100u << head) | (uint32_t)ffs32(o->link) << 6 | o->or);
    uint32_t data = nvbios_outp_match(&d->bios, o->hasht, m, t);
    if (!data) nv_log("nvdisp: no IED table for %04x:%04x\n", o->hasht, m);
    return data;
}

static void ied_on(struct nv_device *d, int head, int sor, int id, uint32_t khz)
{
    int pos = (int)d->ms.sor_asy[sor] - 1;
    if (pos < 0) { nv_log("nvdisp: SOR %d: nothing to attach\n", sor); return; }
    struct nvbios_outp t;
    struct nvbios_ocfg c;
    if (!iedt(d, head, pos, &t)) return;
    uint8_t flags = X.sor_asyst[sor].link == 3 ? 0x01 : 0x00;
    if (!nvbios_ocfg_match(&d->bios, &t, X.sor_asyst[sor].proto_evo, flags, &c)) {
        nv_log("nvdisp: DCB %d: no IEDT config for %02x:%02x\n", pos, X.sor_asyst[sor].proto_evo, flags);
        return;
    }
    uint16_t script = nvbios_oclk_match(&d->bios, c.clkcmp[id], khz);
    if (!script) { nv_log("nvdisp: DCB %d: no OnInt%d script for %u kHz\n", pos, id + 2, khz); return; }
    run_script(d, script, pos, sor, X.sor_asyst[sor].link, head, id ? "IED OnInt3" : "IED OnInt2");
}

static void ied_off(struct nv_device *d, int head, int sor, int id)
{
    int pos = (int)d->ms.sor_arm[sor] - 1;
    if (pos < 0) { nv_log("nvdisp: SOR %d: nothing attached\n", sor); return; }
    struct nvbios_outp t;
    if (!iedt(d, head, pos, &t)) return;
    run_script(d, t.script[id], pos, sor, X.sor_armst[sor].link, head, id == 1 ? "IED OffInt1" : "IED OffInt2");
}

static void route_set(struct nv_device *d, int pos, int sor)
{
    const struct nvbios_output *o = &d->bios.output[pos];
    const uint32_t moff = (uint32_t)(ffs32(o->or) - 1) * 0x100u;
    const uint32_t sorv = sor >= 0 ? (uint32_t)sor + 1u : 0u;
    uint32_t link = sor >= 0 ? (X.sor_asyst[sor].link == 2) : 0;
    if (o->link & 1) { nv_mask(d, 0x612308 + moff, 0x0000001f, link << 4 | sorv); link++; }
    if (o->link & 2) nv_mask(d, 0x612388 + moff, 0x0000001f, link << 4 | sorv);
    nv_log("nvdisp: route DCB %d -> SOR %d\n", pos, sor);
}

static void outp_route(struct nv_device *d)
{
    for (int s = 0; s < NV_MAX_SORS; s++) {
        uint32_t arm = d->ms.sor_arm[s];
        if (arm && arm != d->ms.sor_asy[s]) { route_set(d, (int)arm - 1, -1); d->ms.sor_arm[s] = 0; }
    }
    for (int s = 0; s < NV_MAX_SORS; s++) {
        uint32_t asy = d->ms.sor_asy[s];
        if (asy && asy != d->ms.sor_arm[s]) { route_set(d, (int)asy - 1, s); d->ms.sor_arm[s] = asy; }
    }
}

static void super_2_2_dp(struct nv_device *d, int head, int sor, int pos)
{
    const typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    const struct head_st *a = &X.head_asy[head];
    const uint32_t khz = a->hz / 1000u, hoff = (uint32_t)head * 0x800u;
    if (!khz || !s->nr) return;
    const int64_t linkKBps = (int64_t)s->bw * 27000;
    const int64_t symbol = 100000;

    int64_t h = (int64_t)a->hblanke + a->htotal - a->hblanks - 7;
    h = h * linkKBps / khz - 3 * (s->ef ? 1 : 0) - 12 / s->nr;
    int64_t v = (int64_t)a->vblanks - a->vblanke - 25;
    v = v * linkKBps / khz - (36 / s->nr + 3) - 1;
    nv_mask(d, 0x616620 + hoff, 0x0000ffff, (uint32_t)h & 0xffff);
    nv_mask(d, 0x616624 + hoff, 0x00ffffff, (uint32_t)v & 0xffffff);

    int64_t link_data_rate = ((int64_t)khz * a->depth / 8) / s->nr;
    int64_t link_ratio = link_data_rate * symbol / linkKBps;
    int64_t unk = (symbol - link_ratio) * 64;                /* gm200: no activesym, TU = 64 */
    unk = unk * link_ratio / symbol / symbol + 6;
    nv_mask(d, 0x616610 + hoff, 0x0800003f, 0x08000000u | ((uint32_t)unk & 0x3f));
    nv_log("nvdisp: head %d DP: audio sym h %ld v %ld, watermark %ld\n", head, (long)h, (long)v, (long)unk);
}

static void dp_acquire(struct nv_device *d, int sor, int pos)
{
    typeof(d->ms.dp[0]) *s = &d->ms.dp[pos];
    uint32_t datakbps = 0;
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (X.sor_asyst[sor].heads & (1u << h)) datakbps += (X.head_asy[h].hz / 1000u) * (uint32_t)X.head_asy[h].depth;
    uint32_t dataKBps = (datakbps + 7) / 8, linkKBps = (uint32_t)s->bw * 27000u * s->nr;
    bool retrain = true;
    if (linkKBps >= dataKBps && s->lt_done) {
        uint8_t stat[3];
        if (dp_rd(d, pos, DPCD_LANE0_1_STATUS, stat, 3) == NV_OK && (stat[2] & 0x01)) {
            retrain = false;
            for (int i = 0; i < s->nr; i++) {
                uint8_t lane = (uint8_t)((stat[i >> 1] >> ((i & 1) * 4)) & 0x0f);
                if ((lane & 0x07) != 0x07) retrain = true;
            }
        }
    }
    if (retrain) {
        int rc = dp_train(d, pos, sor, X.sor_asyst[sor].link, dataKBps);
        for (int i = 0; i < d->ms.nouts; i++)
            if (d->ms.out[i].dcb == pos) {
                d->ms.out[i].train_rc = rc;
                d->ms.out[i].dp_bw = s->bw;
                d->ms.out[i].dp_nr = s->nr;
                d->ms.out[i].dp_ef = s->ef;
            }
    }
}

static void super_service(struct nv_device *d)
{
    uint32_t stat = nv_rd32(d, 0x6100ac);
    if (!(stat & 7)) return;
    const uint32_t pending = stat & 7;
    nv_wr32(d, 0x6100ac, pending);
    uint32_t mask[NV_MAX_HEADS] = { 0 };
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (d->head_mask & (1u << h)) mask[h] = nv_rd32(d, 0x6101d4 + (uint32_t)h * 0x800u);
    nv_log("nvdisp: supervisor %08x, head masks %08x %08x %08x %08x\n", pending, mask[0], mask[1], mask[2], mask[3]);

    if (pending & 1) {
        d->ms.supervisors[0]++;
        super_state(d);
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(mask[h] & 0x1000)) continue;
            int sor = ior_arm(d, h);
            if (sor >= 0) ied_off(d, h, sor, 1);
        }
    } else if (pending & 2) {
        d->ms.supervisors[1]++;
        if (!X.state_valid) super_state(d);
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(mask[h] & 0x1000)) continue;
            int sor = ior_arm(d, h);
            if (sor < 0) continue;
            ied_off(d, h, sor, 2);
            int pos = (int)d->ms.sor_arm[sor] - 1;
            if (X.sor_armst[sor].heads == (1u << h) && pos >= 0 && d->bios.output[pos].type == DCB_OUTPUT_DP)
                dp_disable(d, pos, sor, X.sor_armst[sor].link);
        }
        outp_route(d);
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(mask[h] & 0x10000)) continue;
            uint32_t khz = X.head_asy[h].hz / 1000u;
            nv_log("nvdisp: supervisor 2.1 head %d: %u kHz\n", h, khz);
            if (khz) vpll_set(d, h, khz);
        }
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(mask[h] & 0x1000)) continue;
            int sor = ior_asy(d, h);
            if (sor < 0) { nv_log("nvdisp: supervisor 2.2 head %d: no SOR\n", h); continue; }
            int pos = (int)d->ms.sor_asy[sor] - 1;
            uint32_t khz = X.head_asy[h].hz / 1000u;
            bool dp = pos >= 0 && X.sor_asyst[sor].dp && d->bios.output[pos].type == DCB_OUTPUT_DP;
            if (dp) dp_acquire(d, sor, pos);
            ied_on(d, h, sor, 0, khz);
            nv_mask(d, 0x612200 + (uint32_t)h * 0x800u, 0x0000000f, 0);       /* rgclk div 0 */
            if (dp) super_2_2_dp(d, h, sor, pos);
            uint32_t div = X.sor_asyst[sor].link == 3;                        /* gf119_sor_clock */
            nv_mask(d, 0x612300 + sor_soff(sor), 0x00000707, div << 8 | div);
        }
    } else {
        d->ms.supervisors[2]++;
        if (!X.state_valid) super_state(d);
        for (int h = 0; h < NV_MAX_HEADS; h++) {
            if (!(mask[h] & 0x1000)) continue;
            int sor = ior_asy(d, h);
            if (sor >= 0) ied_on(d, h, sor, 1, X.head_asy[h].hz / 1000u);
        }
        X.state_valid = false;
    }
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (d->head_mask & (1u << h)) nv_wr32(d, 0x6101d4 + (uint32_t)h * 0x800u, 0);
    nv_wr32(d, 0x6101d0, 0x80000000);
}

/* UPDATE on the core channel (core507d_update with notifier), then poll:
 * supervisor, exceptions, notifier, channel idle. */
static int core_update(struct nv_device *d, uint32_t interlock, uint32_t timeout_ms)
{
    struct nv_evo_chan *c = &X.core;
    nv_vram_wr32(d, d->ms.sync, 0);
    evo_mthd1(d, c, NV907D_SET_NOTIFIER_CONTROL, NV907D_SET_NOTIFIER_CONTROL_NOTIFY_ENABLE);
    evo_mthd1(d, c, NV907D_UPDATE, interlock);
    evo_mthd1(d, c, NV907D_SET_NOTIFIER_CONTROL, 0);
    int rc = evo_kick(d, c);
    if (rc) return rc;
    d->ms.updates++;
    for (uint32_t t = 0; t < timeout_ms * 100u; t++) {
        super_service(d);
        chan_errors(d);
        if ((nv_vram_rd32(d, d->ms.sync) & 1u) && evo_idle(d, c)) {
            bool idle = true;
            for (int h = 0; h < NV_MAX_HEADS; h++) if (X.base[h].up && !evo_idle(d, &X.base[h])) idle = false;
            if (idle) { nv_log("nvdisp: update %u complete\n", d->ms.updates); return NV_OK; }
        }
        nv_udelay(10);
    }
    nv_log("nvdisp: update %u TIMEOUT: notifier %08x, core GET %08x PUT %08x, ctrl %08x, 0x6100ac %08x, 0x61009c %08x\n",
           d->ms.updates, nv_vram_rd32(d, d->ms.sync), nv_rd32(d, chan_get_reg(c)), c->put,
           nv_rd32(d, chan_ctrl_reg(c)), nv_rd32(d, 0x6100ac), nv_rd32(d, 0x61009c));
    return NV_ERR_TIMEOUT;
}

/* ======================================================================== */
/*  modes                                                                      */
/* ======================================================================== */
static bool edid_dtd(const uint8_t *e, struct nv_mode *m, const char *source)
{
    uint32_t clk = (uint32_t)(e[0] | e[1] << 8) * 10u;
    if (!clk || (e[17] & 0x80)) return false;               /* display descriptor / interlaced */
    uint16_t hact = (uint16_t)(e[2] | (e[4] & 0xf0) << 4), hbl = (uint16_t)(e[3] | (e[4] & 0x0f) << 8);
    uint16_t vact = (uint16_t)(e[5] | (e[7] & 0xf0) << 4), vbl = (uint16_t)(e[6] | (e[7] & 0x0f) << 8);
    uint16_t hso = (uint16_t)(e[8] | (e[11] & 0xc0) << 2), hsw = (uint16_t)(e[9] | (e[11] & 0x30) << 4);
    uint16_t vso = (uint16_t)((e[10] >> 4) | (e[11] & 0x0c) << 2), vsw = (uint16_t)((e[10] & 0x0f) | (e[11] & 0x03) << 4);
    if (!hact || !vact || !hsw || !vsw || hso + hsw > hbl || vso + vsw > vbl) return false;
    m->clock_khz   = clk;
    m->hdisplay    = hact;
    m->hsync_start = (uint16_t)(hact + hso);
    m->hsync_end   = (uint16_t)(hact + hso + hsw);
    m->htotal      = (uint16_t)(hact + hbl);
    m->vdisplay    = vact;
    m->vsync_start = (uint16_t)(vact + vso);
    m->vsync_end   = (uint16_t)(vact + vso + vsw);
    m->vtotal      = (uint16_t)(vact + vbl);
    if ((e[17] & 0x18) == 0x18) { m->nvsync = !(e[17] & 0x04); m->nhsync = !(e[17] & 0x02); }
    else { m->nvsync = m->nhsync = false; }
    m->source = source;
    return true;
}

static bool mode_fits(const struct nv_mode *m, int maxw, int maxh, uint32_t max_kBps)
{
    return m->hdisplay <= maxw && m->vdisplay <= maxh && m->clock_khz * 3u <= max_kBps && m->clock_khz <= 600000;
}

static bool pick_mode(struct nv_device *d, int pos, int maxw, int maxh, uint32_t max_kBps, struct nv_mode *out)
{
    static const char *const names[] = { "EDID preferred", "EDID DTD 2", "EDID DTD 3", "EDID DTD 4",
                                         "EDID ext DTD 1", "EDID ext DTD 2", "EDID ext DTD 3", "EDID ext DTD 4",
                                         "EDID ext DTD 5", "EDID ext DTD 6" };
    static const struct nv_mode fallback[] = {
        { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, false, false, "CEA 1080p60" },
        {  74250, 1280, 1390, 1430, 1650,  720,  725,  730,  750, false, false, "CEA 720p60" },
        {  65000, 1024, 1048, 1184, 1344,  768,  771,  777,  806, true,  true,  "DMT 1024x768@60" },
    };
    const struct nv_dp_probe *p = &d->dp[pos];
    struct nv_mode cand[10];
    int n = 0;
    if (p->edid_rc == NV_OK && p->edid_len >= 128) {
        for (int i = 0; i < 4; i++) {
            struct nv_mode m;
            if (edid_dtd(p->edid + 54 + i * 18, &m, names[n])) cand[n++] = m;
        }
        if (p->edid_len >= 256 && p->edid[128] == 0x02 && p->edid[130] >= 4) {
            for (uint32_t o = 128u + p->edid[130]; o + 18 <= 255 && n < 10; o += 18) {
                struct nv_mode m;
                if (!edid_dtd(p->edid + o, &m, names[n])) break;
                cand[n++] = m;
            }
        }
    }
    for (int i = 0; i < n; i++)
        nv_log("nvdisp: DCB %d: %s: %ux%u, %u kHz, h %u %u %u, v %u %u %u%s\n", pos, cand[i].source, cand[i].hdisplay,
               cand[i].vdisplay, cand[i].clock_khz, cand[i].hsync_start, cand[i].hsync_end, cand[i].htotal,
               cand[i].vsync_start, cand[i].vsync_end, cand[i].vtotal,
               mode_fits(&cand[i], maxw, maxh, max_kBps) ? "" : " (too big)");
    if (n && mode_fits(&cand[0], maxw, maxh, max_kBps)) { *out = cand[0]; return true; }
    int best = -1;
    for (int i = 1; i < n; i++)
        if (mode_fits(&cand[i], maxw, maxh, max_kBps) &&
            (best < 0 || (uint32_t)cand[i].hdisplay * cand[i].vdisplay > (uint32_t)cand[best].hdisplay * cand[best].vdisplay))
            best = i;
    if (best >= 0) { *out = cand[best]; return true; }
    for (size_t i = 0; i < sizeof fallback / sizeof *fallback; i++)
        if (mode_fits(&fallback[i], maxw, maxh, max_kBps)) { *out = fallback[i]; return true; }
    return false;
}

/* ======================================================================== */
/*  VRAM placement                                                             */
/* ======================================================================== */
static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

static uint64_t choose_base(struct nv_device *d, uint64_t need)
{
    uint64_t busy[16];
    int nb = 0;
    uint32_t v;
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (d->head[h].active) busy[nb++] = (uint64_t)d->head[h].offset << 8;
    if ((v = nv_rd32(d, 0x610010)) & 0xffffff00u) busy[nb++] = (uint64_t)(v & 0xffffff00u) << 8;
    if ((v = nv_rd32(d, 0x619f04)) & 0x00000008u) busy[nb++] = (uint64_t)(v & 0xffffff00u) << 8;
    if ((v = nv_rd32(d, 0x001704)) & 0x0fffffffu) busy[nb++] = (uint64_t)(v & 0x0fffffffu) << 12;
    if ((v = nv_rd32(d, 0x001714)) & 0x0fffffffu) busy[nb++] = (uint64_t)(v & 0x0fffffffu) << 12;
    for (int i = 0; i < nb; i++) nv_log("nvdisp: firmware uses VRAM near %lx\n", (unsigned long)busy[i]);

    const uint64_t guard = 32ull << 20;
    for (uint64_t base = 256ull << 20; base + need + (512ull << 20) <= d->vram_bytes; base += 256ull << 20) {
        bool ok = true;
        for (int i = 0; i < nb && ok; i++)
            if (overlaps(base, need, busy[i] > guard ? busy[i] - guard : 0, 2 * guard)) ok = false;
        if (ok) return base;
    }
    return 0;
}

/* ======================================================================== */
/*  display engine takeover (gf119_disp_init, nvkm_disp_init)                   */
/* ======================================================================== */
static int disp_init(struct nv_device *d)
{
    for (int h = 0; h < NV_MAX_HEADS; h++) {
        if (!(d->head_mask & (1u << h))) continue;
        const uint32_t hoff = (uint32_t)h * 0x800u;
        nv_wr32(d, 0x6101b4 + hoff, nv_rd32(d, 0x616104 + hoff));
        nv_wr32(d, 0x6101b8 + hoff, nv_rd32(d, 0x616108 + hoff));
        nv_wr32(d, 0x6101bc + hoff, nv_rd32(d, 0x61610c + hoff));
    }
    for (int s = 0; s < NV_MAX_SORS; s++)
        if (d->sor_mask & (1u << s)) nv_wr32(d, 0x6301c4 + sor_soff(s), nv_rd32(d, 0x61c000 + sor_soff(s)));

    uint32_t owner = nv_rd32(d, 0x6100ac);
    nv_log("nvdisp: 0x6100ac %08x, 0x6194e8 %08x, 0x610010 %08x, core ctrl %08x\n", owner, nv_rd32(d, 0x6194e8),
           nv_rd32(d, 0x610010), nv_rd32(d, 0x610490));
    if (owner & 0x00000100) {                               /* steal display from the VBIOS */
        nv_wr32(d, 0x6100ac, 0x00000100);
        nv_mask(d, 0x6194e8, 0x00000001, 0x00000000);
        if (!nv_wait(d, 0x6194e8, 0x00000002, 0, 2000000)) {
            nv_log("nvdisp: VBIOS did not release the display (0x6194e8 %08x)\n", nv_rd32(d, 0x6194e8));
            return NV_ERR_TIMEOUT;
        }
    }

    pramin_invalidate();
    nv_vram_fill(d, d->ms.inst, 0, DISP_INST_SIZE);
    nv_vram_fill(d, d->ms.sync, 0, 0x1000);
    X.obj_next = DISP_OBJ_BASE;
    nv_wr32(d, 0x610010, (uint32_t)(d->ms.inst >> 8) | 0x9);

    nv_wr32(d, 0x610090, 0x00000000);
    nv_wr32(d, 0x6100a0, 0x00000000);
    nv_wr32(d, 0x6100b0, 0x00000307);
    for (int h = 0; h < NV_MAX_HEADS; h++)
        if (d->head_mask & (1u << h)) nv_mask(d, 0x616308 + (uint32_t)h * 0x800u, 0x00000111, 0x00000010);

    for (int s = 0; s < NV_MAX_SORS; s++) {                 /* nv50_sor_power: normal, powered up */
        if (!(d->sor_mask & (1u << s))) continue;
        const uint32_t soff = sor_soff(s);
        nv_wait(d, 0x61c004 + soff, 0x80000000, 0, 2000000);
        nv_mask(d, 0x61c004 + soff, 0x80000001, 0x80000001);
        nv_wait(d, 0x61c004 + soff, 0x80000000, 0, 2000000);
        if (!nv_wait(d, 0x61c030 + soff, 0x10000000, 0, 2000000))
            nv_log("nvdisp: SOR %d power state timeout (%08x)\n", s, nv_rd32(d, 0x61c030 + soff));
    }
    return NV_OK;
}

/* ======================================================================== */
/*  planning                                                                   */
/* ======================================================================== */
static int plan(struct nv_device *d)
{
    struct nv_modeset *ms = &d->ms;
    uint32_t heads_used = 0, sors_used = 0;

    /* what the firmware lit: SOR -> DCB output through the route registers */
    for (int s = 0; s < NV_MAX_SORS; s++) {
        if (!(d->sor_mask & (1u << s)) || !d->sor[s].heads) continue;
        sors_used |= 1u << s;
        heads_used |= d->sor[s].heads;
        for (int pos = 0; pos < d->bios.noutputs; pos++)
            if (d->dp[pos].route_sor == s) ms->sor_arm[s] = (uint32_t)pos + 1;
        nv_log("nvdisp: firmware: SOR %d drives heads %x (DCB %d)\n", s, d->sor[s].heads, (int)ms->sor_arm[s] - 1);
    }

    for (int pos = 0; pos < d->bios.noutputs && ms->nouts < NV_MAX_HEADS; pos++) {
        const struct nvbios_output *o = &d->bios.output[pos];
        if (o->type != DCB_OUTPUT_DP || o->location || !d->dp[pos].sink) continue;
        int lit_sor = -1;
        for (int s = 0; s < NV_MAX_SORS; s++) if (ms->sor_arm[s] == (uint32_t)pos + 1) lit_sor = s;

        struct nv_output *out = &ms->out[ms->nouts];
        mzero(out, sizeof *out);
        out->dcb = pos;
        out->head = out->sor = -1;
        out->train_rc = 1;

        if (lit_sor >= 0 && !X.opts.takeover) {
            out->kept = true;
            out->sor = lit_sor;
            out->head = ffs32(d->sor[lit_sor].heads) - 1;
            out->link = X.sor_armst[lit_sor].link ? X.sor_armst[lit_sor].link : (d->sor[lit_sor].proto == 9 ? 2 : 1);
            const struct nv_head_state *hs = &d->head[out->head];
            out->mode.hdisplay = (uint16_t)(hs->size & 0xffff);
            out->mode.vdisplay = (uint16_t)(hs->size >> 16);
            out->mode.clock_khz = hs->pixel_hz / 1000u;
            out->mode.source = "firmware";
            out->fb = (uint64_t)hs->offset << 8;
            out->pitch = ((hs->storage >> 8) & 0x1fff) << 8;
            out->status = "kept as the firmware left it";
            ms->sor_asy[lit_sor] = (uint32_t)pos + 1;
            ms->nouts++;
            continue;
        }
        if (!dp_enable(d, pos)) { nv_log("nvdisp: DCB %d: DP sink not usable, skipped\n", pos); continue; }

        uint32_t free_heads = d->head_mask & o->heads & ~heads_used;
        if (lit_sor >= 0) free_heads |= d->sor[lit_sor].heads & d->head_mask & o->heads;
        if (!free_heads) { nv_log("nvdisp: DCB %d: no free head (allowed %x, used %x)\n", pos, o->heads, heads_used); continue; }
        int head = ffs32(free_heads) - 1;

        int sor = lit_sor;
        if (sor < 0) {
            int r = d->dp[pos].route_sor;
            if (r >= 0 && r < NV_MAX_SORS && (d->sor_mask & (1u << r)) && !(sors_used & (1u << r))) sor = r;
            for (int s = 0; sor < 0 && s < NV_MAX_SORS; s++)
                if ((d->sor_mask & (1u << s)) && !(sors_used & (1u << s))) sor = s;
        }
        if (sor < 0) { nv_log("nvdisp: DCB %d: no free SOR\n", pos); continue; }

        const typeof(ms->dp[0]) *dp = &ms->dp[pos];
        uint32_t max_kBps = (uint32_t)dp->rate[0] * 27000u * dp->links;
        if (!pick_mode(d, pos, X.opts.max_width, X.opts.max_height, max_kBps, &out->mode)) {
            nv_log("nvdisp: DCB %d: no usable mode\n", pos);
            continue;
        }
        out->head = head;
        out->sor = sor;
        out->link = (o->link & 2) && !(o->link & 1) ? 2 : 1;
        out->pitch = surface_pitch(out->mode.hdisplay);
        out->status = "planned";
        heads_used |= 1u << head;
        sors_used |= 1u << sor;
        ms->sor_asy[sor] = (uint32_t)pos + 1;
        nv_log("nvdisp: DCB %d: head %d, SOR %d link %d, %ux%u @ %u kHz (%s)\n", pos, head, sor, out->link,
               out->mode.hdisplay, out->mode.vdisplay, out->mode.clock_khz, out->mode.source);
        ms->nouts++;
    }
    int fresh = 0;
    for (int i = 0; i < ms->nouts; i++) fresh += !ms->out[i].kept;
    return fresh;
}

/* ======================================================================== */
/*  nv_modeset                                                                 */
/* ======================================================================== */
static int fail(struct nv_device *d, int rc, const char *why)
{
    d->ms.result = rc;
    nv_log("nvdisp: modeset stopped at '%s': %s (%s)\n", d->ms.stage ? d->ms.stage : "start", why, nv_strerror(rc));
    if (nv_plat->checkpoint) nv_plat->checkpoint("modeset failed");
    return rc;
}

int nv_modeset(struct nv_device *d, const struct nv_modeset_opts *opts)
{
    struct nv_modeset *ms = &d->ms;
    mzero(ms, sizeof *ms);
    mzero(&X, sizeof X);
    ms->attempted = true;
    X.d = d;
    if (opts) X.opts = *opts;
    if (X.opts.max_width <= 0)  X.opts.max_width = 1920;
    if (X.opts.max_height <= 0) X.opts.max_height = 1200;
    script_ops_init(d);

    checkpoint(d, "modeset: start");
    if (d->chipset < 0x132 || d->chipset > 0x138 || d->chipset == 0x130)
        return fail(d, NV_ERR_NOSUPP, "only GP102-GP108 (GP102-class display) are supported");
    if (!d->display_present || !d->bios_ok || !d->aux_supported || !d->mmio || !d->vram_bytes)
        return fail(d, NV_ERR_NODEV, "display, VBIOS, AUX or VRAM missing");
    ms->crystal_khz = crystal_khz(d);
    nv_log("nvdisp: %s, crystal %u kHz, VRAM %lu MiB, DP table version %02x, max mode %dx%d%s\n", d->chip_name,
           ms->crystal_khz, (unsigned long)(d->vram_bytes >> 20), nvbios_dp_version(&d->bios),
           X.opts.max_width, X.opts.max_height, X.opts.takeover ? ", takeover" : "");

    for (int s = 0; s < NV_MAX_SORS; s++) if (d->sor_mask & (1u << s)) ior_state(d, s, false, &X.sor_armst[s]);
    int fresh = plan(d);
    checkpoint(d, "modeset: planned");
    if (fresh <= 0) {
        ms->result = NV_OK;
        nv_log("nvdisp: nothing to light (%d output(s) kept)\n", ms->nouts);
        return NV_OK;
    }

    uint64_t need = AREA_FB;
    for (int i = 0; i < ms->nouts; i++)
        if (!ms->out[i].kept)
            need += ((uint64_t)ms->out[i].pitch * ms->out[i].mode.vdisplay + FB_ALIGN - 1) & ~(FB_ALIGN - 1);
    if (need > AREA_MAX) return fail(d, NV_ERR_NOMEM, "frame buffers too large");
    ms->base = choose_base(d, need);
    if (!ms->base) return fail(d, NV_ERR_NOMEM, "no free VRAM area away from the firmware's");
    ms->inst = ms->base + AREA_INST;
    ms->push = ms->base + AREA_CORE_PUSH;
    ms->sync = ms->base + AREA_SYNC;
    ms->fb_next = ms->base + AREA_FB;
    nv_log("nvdisp: our VRAM area %lx-%lx\n", (unsigned long)ms->base, (unsigned long)(ms->base + need - 1));

    /* frame buffers, cleared before anything scans them out */
    pramin_invalidate();
    for (int i = 0; i < ms->nouts; i++) {
        struct nv_output *o = &ms->out[i];
        if (o->kept) continue;
        uint64_t size = (uint64_t)o->pitch * o->mode.vdisplay;
        o->fb = ms->fb_next;
        ms->fb_next += (size + FB_ALIGN - 1) & ~(FB_ALIGN - 1);
        nv_vram_fill(d, o->fb, 0, (uint32_t)size);
        nv_log("nvdisp: DCB %d: frame buffer %lx, pitch %u, %lu KiB\n", o->dcb, (unsigned long)o->fb, o->pitch,
               (unsigned long)(size >> 10));
    }

    /* dark DP outputs routed to an idle SOR: DisableLT (nvkm_outp_init_route) */
    for (int pos = 0; pos < d->bios.noutputs; pos++) {
        int r = d->dp[pos].route_sor;
        if (d->bios.output[pos].type != DCB_OUTPUT_DP || r < 0 || r >= NV_MAX_SORS || d->sor[r].heads) continue;
        dp_disable(d, pos, r, X.sor_armst[r].link ? X.sor_armst[r].link : 1);
    }

    checkpoint(d, "modeset: taking the display over");
    int rc = disp_init(d);
    if (rc) return fail(d, rc, "display takeover");

    X.core.name = "core";
    X.core.ctrl = EVO_CORE_CTRL;
    X.core.user = EVO_CORE_USER;
    X.core.core = true;
    X.core.push = ms->push;
    if ((rc = bind_channel_objects(d, EVO_CORE_USER, true))) return fail(d, rc, "core channel objects");
    if ((rc = evo_chan_init(d, &X.core))) return fail(d, rc, "core channel init");
    evo_mthd1(d, &X.core, NV907D_SET_CONTEXT_DMA_NOTIFIER, NV_HANDLE_SYNC);
    checkpoint(d, "modeset: core channel up");

    /* adopt: replay every head and SOR the firmware armed */
    for (int s = 0; s < NV_MAX_SORS; s++) {
        if (!(d->sor_mask & (1u << s)) || !d->sor[s].heads) continue;
        for (int h = 0; h < NV_MAX_HEADS; h++)
            if (d->sor[s].heads & (1u << h)) core_head_adopt(d, &X.core, h);
        core_sor_adopt(d, &X.core, s);
    }
    if ((rc = core_update(d, 0, 2000))) return fail(d, rc, "adopting the firmware state");
    checkpoint(d, "modeset: firmware state adopted");

    /* light: base channels, then one interlocked update */
    uint32_t interlock = 0;
    for (int i = 0; i < ms->nouts; i++) {
        struct nv_output *o = &ms->out[i];
        if (o->kept) continue;
        if ((rc = base_create(d, o->head))) { o->status = "base channel failed"; return fail(d, rc, "base channel"); }
    }
    for (int i = 0; i < ms->nouts; i++) {
        struct nv_output *o = &ms->out[i];
        if (o->kept) continue;
        base_image(d, &X.base[o->head], o);
        if ((rc = evo_kick(d, &X.base[o->head]))) return fail(d, rc, "base channel kick");
        core_head_program(d, &X.core, o);
        interlock |= NV907D_UPDATE_INTERLOCK_WITH_BASE(o->head);
        o->status = "programmed";
    }
    checkpoint(d, "modeset: lighting new heads");
    if ((rc = core_update(d, interlock, 10000))) return fail(d, rc, "lighting the new heads");
    checkpoint(d, "modeset: update done");

    /* verify */
    int lit = 0;
    for (int i = 0; i < ms->nouts; i++) {
        struct nv_output *o = &ms->out[i];
        if (o->kept) { o->lit = true; lit++; continue; }
        struct head_st hs;
        struct ior_st is;
        head_state(d, o->head, false, &hs);
        ior_state(d, o->sor, false, &is);
        uint8_t stat[3] = { 0 };
        dp_rd(d, o->dcb, DPCD_LANE0_1_STATUS, stat, 3);
        bool lanes_ok = stat[2] & 0x01;
        for (int l = 0; l < o->dp_nr; l++) if (((stat[l >> 1] >> ((l & 1) * 4)) & 0x07) != 0x07) lanes_ok = false;
        o->lit = (is.heads & (1u << o->head)) && hs.hz == o->mode.clock_khz * 1000u && lanes_ok && o->train_rc == 0;
        o->status = o->lit ? "lit" : (o->train_rc ? "link training failed" : "not armed as programmed");
        lit += o->lit;
        nv_log("nvdisp: DCB %d head %d: armed %ux%u total, %u Hz, SOR %d heads %x proto %x, lanes %02x %02x %02x: %s\n",
               o->dcb, o->head, hs.htotal, hs.vtotal, hs.hz, o->sor, is.heads, is.proto_evo, stat[0], stat[1], stat[2],
               o->status);
    }
    ms->result = lit ? NV_OK : NV_ERR_STATE;
    checkpoint(d, "modeset: finished");
    return ms->result;
}

/* ======================================================================== */
/*  report                                                                     */
/* ======================================================================== */
size_t nv_modeset_report(const struct nv_device *d, char *buf, size_t cap)
{
    const struct nv_modeset *ms = &d->ms;
    struct nv_sbuf b = { buf, 0, cap, 0 };
    if (!ms->attempted) {
        nv_sb_printf(&b, "modeset    not attempted (boot with nvidia.modeset=1)\n");
        return b.n < cap ? b.n : cap;
    }
    nv_sb_printf(&b, "modeset    %s, last stage '%s'\n", nv_strerror(ms->result), ms->stage ? ms->stage : "-");
    nv_sb_printf(&b, "           VRAM area %lx, instance memory %lx, core push %lx, notifier %lx\n",
                 (unsigned long)ms->base, (unsigned long)ms->inst, (unsigned long)ms->push, (unsigned long)ms->sync);
    nv_sb_printf(&b, "           crystal %u kHz; %u update(s); supervisors %u/%u/%u; channel exceptions %u\n",
                 ms->crystal_khz, ms->updates, ms->supervisors[0], ms->supervisors[1], ms->supervisors[2], ms->chan_errors);
    nv_sb_printf(&b, "           VBIOS scripts: %u run, %u register writes, %u warnings\n",
                 ms->script_runs, ms->script_writes, ms->script_warnings);
    for (int i = 0; i < ms->nouts; i++) {
        const struct nv_output *o = &ms->out[i];
        nv_sb_printf(&b, "  DCB %d: head %d, SOR %d link %d, %ux%u @ %u kHz (%s), fb %lx pitch %u",
                     o->dcb, o->head, o->sor, o->link, o->mode.hdisplay, o->mode.vdisplay, o->mode.clock_khz,
                     o->mode.source ? o->mode.source : "-", (unsigned long)o->fb, o->pitch);
        if (!o->kept) nv_sb_printf(&b, ", DP %u x %02x%s, training %d", o->dp_nr, o->dp_bw, o->dp_ef ? " EF" : "", o->train_rc);
        nv_sb_printf(&b, ": %s\n", o->status ? o->status : "-");
    }
    return b.n < cap ? b.n : cap;
}
