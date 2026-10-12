/* drivers/sound/hda.c -- Intel High Definition Audio: controller and codecs
 *
 * The controller (PCI class 04.03: Intel ICH/PCH, AMD, NVIDIA, VIA, and
 * QEMU's `-device intel-hda` / `ich9-intel-hda`) talks to its codecs over
 * the HDA link: verbs go out through the CORB ring and answers come back in
 * the RIRB ring, both in memory. Each codec is a tree of widgets: an audio
 * function group holding converters (DACs, ADCs), mixers, selectors and pin
 * complexes (the jacks and the built-in speaker). The generic parser below
 * reads that tree and, for every output pin the board wires up (speaker,
 * headphone, line out; from the pin's configuration default, which the
 * firmware sets), finds a path back to a DAC, selects the connections on
 * the way, unmutes the amplifiers and turns the pin's output and external
 * amplifier (EAPD) on. All the DACs found play the same stream.
 *
 * Realtek ALC codecs (vendor 10ec, the usual codec on PC boards) are
 * handled by the same parser; their speaker and headphone pins need EAPD,
 * and some keep the headphone amplifier off until the pin's HP bit is set.
 *
 * Playback runs on one output stream descriptor: a ring of 64 KiB in eight
 * buffer descriptor list entries, played over and over. Interrupts stay off;
 * the position comes from the DMA position buffer (or the stream's LPIB
 * register), and a small kernel thread zeroes what has been played so a
 * stalled writer plays silence, not the old sound again.
 *
 * Programs get the OSS interface: /dev/dsp (write PCM; S16_LE, U8, S32_LE,
 * mono or stereo, the rates the codec supports) and /dev/mixer (playback
 * volume). The hardware always runs 16-bit stereo; other formats are
 * converted on the way in.
 *
 * Locking, without the big kernel lock: `slock` (a spinlock) guards the
 * stream and the codec link (the ring accounting, starting and stopping,
 * verbs); `dsp_mutex` serialises /dev/dsp's writers and its ioctls, which
 * share the conversion state and may sleep. The kernel thread lets go of
 * the big lock; writes and polls of /dev/dsp run without it. */
#include <kernel/sound.h>
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/klog.h>
#include <kernel/cpu.h>
#include <kernel/time.h>
#include <kernel/string.h>
#include <kernel/uaccess.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include "../pci/pci.h"

static struct spinlock slock;           /* the stream and the codec link */
static struct kmutex dsp_mutex;         /* /dev/dsp writers and ioctls   */

/* ---- controller registers ---------------------------------------------------- */
enum {
    GCAP = 0x00, VMIN = 0x02, VMAJ = 0x03, GCTL = 0x08, WAKEEN = 0x0C, STATESTS = 0x0E, INTCTL = 0x20,
    INTSTS = 0x24, CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C,
    CORBSTS = 0x4D, CORBSIZE = 0x4E, RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A,
    RIRBCTL = 0x5C, RIRBSTS = 0x5D, RIRBSIZE = 0x5E, ICOI = 0x60, ICII = 0x64, ICIS = 0x68,
    DPLBASE = 0x70, DPUBASE = 0x74,
};
/* stream descriptor registers, from 0x80 + 0x20 * index */
enum { SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12,
       SD_BDPL = 0x18, SD_BDPU = 0x1C };
#define SD_CTL_SRST  0x01
#define SD_CTL_RUN   0x02

/* ---- codec verbs and parameters ------------------------------------------------ */
#define V_GET_PARAM     0xF00
#define V_GET_CONN_SEL  0xF01
#define V_SET_CONN_SEL  0x701
#define V_GET_CONN_LIST 0xF02
#define V_SET_POWER     0x705
#define V_SET_STREAM    0x706
#define V_SET_PIN_CTL   0x707
#define V_GET_PIN_CTL   0xF07
#define V_GET_PIN_SENSE 0xF09
#define V_SET_EAPD      0x70C
#define V_GET_CFG_DEF   0xF1C
#define V_FUNC_RESET    0x7FF
#define V4_SET_FORMAT   0x2             /* 4-bit verbs carry a 16-bit payload */
#define V4_SET_AMP      0x3
#define V4_GET_AMP      0xB
#define V4_SET_COEF_IDX 0x5
#define V4_SET_PROC_COEF 0x4

#define P_VENDOR        0x00
#define P_NODE_COUNT    0x04
#define P_FG_TYPE       0x05
#define P_WIDGET_CAPS   0x09
#define P_PCM           0x0A
#define P_STREAM_FMTS   0x0B
#define P_PIN_CAPS      0x0C
#define P_AMP_IN        0x0D
#define P_CONN_LEN      0x0E
#define P_AMP_OUT       0x12

enum { W_OUT = 0, W_IN = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4, W_POWER = 5, W_KNOB = 6, W_BEEP = 7 };
#define WC_IN_AMP    (1u << 1)
#define WC_OUT_AMP   (1u << 2)
#define WC_AMP_OVR   (1u << 3)
#define WC_CONN_LIST (1u << 8)
#define WC_DIGITAL   (1u << 9)
#define WC_POWER     (1u << 10)
#define PC_OUT       (1u << 4)
#define PC_HP_DRIVE  (1u << 3)
#define PC_PRESENCE  (1u << 2)
#define PC_EAPD      (1u << 16)
#define PIN_OUT_EN   0x40
#define PIN_HP_EN    0x80

/* configuration default: default device and connectivity */
enum { DEV_LINE_OUT = 0, DEV_SPEAKER = 1, DEV_HP_OUT = 2, DEV_CD = 3, DEV_SPDIF_OUT = 4, DEV_DIG_OUT = 5 };
#define CFG_DEVICE(c)  (((c) >> 20) & 0xf)
#define CFG_CONN(c)    (((c) >> 30) & 0x3)     /* 0 jack, 1 nothing, 2 fixed (built in), 3 both */

#define MAX_NID     128
#define MAX_CONN    32
#define MAX_PATH    8
#define MAX_DACS    4
#define RING_BYTES  (64u * 1024)
#define BDL_N       8
#define CORB_MAX    256

struct widget {
    uint32_t caps, pincaps, cfg, amp_in, amp_out, pcm;
    uint8_t  type, nconn;
    uint8_t  conn[MAX_CONN];
};

struct bdl_entry { uint64_t addr; uint32_t len; uint32_t ioc; };

static struct hda {
    bool present, ready;
    struct pci_device *pci;
    volatile uint8_t *mm;
    uint32_t *corb;
    volatile uint64_t *rirb;
    unsigned corb_n, rirb_n, corb_wp, rirb_rp;
    bool immediate;                     /* CORB/RIRB failed: the immediate command registers */
    volatile uint32_t *posbuf;
    int iss, oss;
    /* the codec in use */
    int caddr;
    uint32_t vendor;
    uint8_t afg;
    struct widget w[MAX_NID];
    uint8_t dacs[MAX_DACS]; int ndacs;
    uint8_t vol_nid[MAX_DACS * 2]; bool vol_out[MAX_DACS * 2]; int nvol;   /* amps the volume control moves */
    char outs[96];
    /* playback */
    uint8_t *ring;
    struct bdl_entry *bdl;
    int sd;                             /* stream descriptor index */
    uint8_t tag;
    bool running, use_lpib;
    uint32_t last_pos;
    uint64_t played, written;           /* bytes, since the stream started */
    uint64_t pos_stuck_since;
    uint32_t rates;                     /* P_PCM rate bits the DAC supports */
    uint16_t fmt;                       /* current stream format */
    int rate;
    int volume;                         /* 0..100, both channels */
} hda;

static uint8_t  r8(uint32_t o)  { return *(volatile uint8_t *)(hda.mm + o); }
static uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(hda.mm + o); }
static uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(hda.mm + o); }
static void w8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(hda.mm + o) = v; }
static void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(hda.mm + o) = v; }
static void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(hda.mm + o) = v; }
static uint32_t sdreg(int sd, uint32_t r) { return 0x80u + 0x20u * (uint32_t)sd + r; }

static bool wait32(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = time_us() + (uint64_t)ms * 1000;
    while ((r32(reg) & mask) != want) { if (time_us() > end) return false; udelay(10); }
    return true;
}
static bool wait16(uint32_t reg, uint16_t mask, uint16_t want, uint32_t ms)
{
    uint64_t end = time_us() + (uint64_t)ms * 1000;
    while ((r16(reg) & mask) != want) { if (time_us() > end) return false; udelay(10); }
    return true;
}
static bool wait8(uint32_t reg, uint8_t mask, uint8_t want, uint32_t ms)
{
    uint64_t end = time_us() + (uint64_t)ms * 1000;
    while ((r8(reg) & mask) != want) { if (time_us() > end) return false; udelay(10); }
    return true;
}

static void *dma_alloc(size_t bytes)
{
    size_t frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_dma32(frames);            /* not every controller does 64-bit DMA */
    if (!phys) return NULL;
    memset((void *)(uintptr_t)phys, 0, frames * PAGE_SIZE);
    return (void *)(uintptr_t)phys;
}

/* ---- the command interface -------------------------------------------------------- */
static int cmd_immediate(uint32_t word, uint32_t *resp)
{
    if (!wait16(ICIS, 1, 0, 10)) return -ETIMEDOUT;      /* ICB: busy */
    w16(ICIS, 2);                                        /* clear IRV */
    w32(ICOI, word);
    w16(ICIS, 1);
    if (!wait16(ICIS, 3, 2, 50)) return -ETIMEDOUT;
    *resp = r32(ICII);
    w16(ICIS, 2);
    return 0;
}

static int cmd_ring(uint32_t word, uint32_t *resp)
{
    hda.corb_wp = (hda.corb_wp + 1) % hda.corb_n;
    hda.corb[hda.corb_wp] = word;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w16(CORBWP, (uint16_t)hda.corb_wp);
    uint64_t end = time_us() + 100000;
    for (;;) {
        unsigned wp = r16(RIRBWP) & 0xff;
        if (hda.rirb_n < 256) wp %= hda.rirb_n;
        while (hda.rirb_rp != wp) {
            hda.rirb_rp = (hda.rirb_rp + 1) % hda.rirb_n;
            uint64_t e = hda.rirb[hda.rirb_rp];
            if ((e >> 32) & 0x10) continue;              /* unsolicited: not asked for */
            *resp = (uint32_t)e;
            w8(RIRBSTS, 0x05);                          /* RINTFL, RIRBOIS */
            return 0;
        }
        if (time_us() > end) return -ETIMEDOUT;
        udelay(5);
    }
}

static int send(int caddr, int nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    uint32_t word = ((uint32_t)caddr << 28) | ((uint32_t)nid << 20);
    word |= verb > 0xF ? (verb << 8) | (payload & 0xff) : (verb << 16) | (payload & 0xffff);
    uint32_t dummy;
    if (!resp) resp = &dummy;
    int r = hda.immediate ? cmd_immediate(word, resp) : cmd_ring(word, resp);
    if (r && !hda.immediate) {
        kprintf("hda: no answer from the command ring, using the immediate command registers\n");
        hda.immediate = true;
        w8(CORBCTL, 0);
        w8(RIRBCTL, 0);
        r = cmd_immediate(word, resp);
    }
    return r;
}

static uint32_t verb(int nid, uint32_t v, uint32_t payload)
{
    uint32_t r = 0;
    if (send(hda.caddr, nid, v, payload, &r)) return 0;
    return r;
}
static uint32_t param(int caddr, int nid, int p)
{
    uint32_t r = 0;
    if (send(caddr, nid, V_GET_PARAM, (uint32_t)p, &r)) return 0;
    return r;
}

/* ---- controller bring-up ----------------------------------------------------------- */
static bool ring_sizes(uint32_t reg, unsigned *n)
{
    uint8_t cap = r8(reg);
    uint8_t sel = (cap & 0x40) ? 2 : (cap & 0x20) ? 1 : 0;
    *n = sel == 2 ? 256 : sel == 1 ? 16 : 2;
    w8(reg, (uint8_t)((cap & 0xf0) | sel));
    return true;
}

static bool controller_reset(void)
{
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait8(CORBCTL, 2, 0, 10);
    wait8(RIRBCTL, 2, 0, 10);
    for (int i = 0; i < hda.iss + hda.oss; i++) w8(sdreg(i, SD_CTL), 0);
    w32(DPLBASE, 0);
    w32(DPUBASE, 0);
    w16(STATESTS, 0x7fff);
    w32(GCTL, r32(GCTL) & ~1u);                         /* CRST low: the link in reset */
    if (!wait32(GCTL, 1, 0, 100)) return false;
    udelay(200);
    w32(GCTL, r32(GCTL) | 1u);
    if (!wait32(GCTL, 1, 1, 100)) return false;
    mdelay(2);                                          /* codecs ask for an address within 521 us */
    return true;
}

static bool rings_start(void)
{
    hda.corb = dma_alloc(CORB_MAX * 4);
    hda.rirb = dma_alloc(CORB_MAX * 8);
    if (!hda.corb || !hda.rirb) return false;
    ring_sizes(CORBSIZE, &hda.corb_n);
    ring_sizes(RIRBSIZE, &hda.rirb_n);
    uint64_t c = (uint64_t)(uintptr_t)hda.corb, r = (uint64_t)(uintptr_t)hda.rirb;
    w32(CORBLBASE, (uint32_t)c); w32(CORBUBASE, (uint32_t)(c >> 32));
    w32(RIRBLBASE, (uint32_t)r); w32(RIRBUBASE, (uint32_t)(r >> 32));
    w16(CORBRP, 0x8000);                                /* reset the read pointer */
    wait16(CORBRP, 0x8000, 0x8000, 5);                  /* not every controller shows the handshake */
    w16(CORBRP, 0);
    wait16(CORBRP, 0x8000, 0, 5);
    w16(CORBWP, 0);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 1);
    hda.corb_wp = 0;
    hda.rirb_rp = 0;
    w8(CORBCTL, 2);                                     /* DMA run */
    w8(RIRBCTL, 3);                                     /* DMA run + response "interrupt": the status
                                                           bit, cleared after each answer, rewinds the
                                                           response counter (no interrupt reaches the
                                                           CPU: INTCTL stays 0) */
    return wait8(CORBCTL, 2, 2, 10) && wait8(RIRBCTL, 2, 2, 10);
}

/* ---- the codec tree ------------------------------------------------------------------ */
static void read_conn_list(int nid, struct widget *w)
{
    uint32_t len = verb(nid, V_GET_PARAM, P_CONN_LEN);
    int n = (int)(len & 0x7f);
    bool lng = len & 0x80;
    int per = lng ? 2 : 4, bits = lng ? 16 : 8;
    uint32_t prev = 0;
    w->nconn = 0;
    for (int i = 0; i < n; i += per) {
        uint32_t e = verb(nid, V_GET_CONN_LIST, (uint32_t)i);
        for (int k = 0; k < per && i + k < n; k++) {
            uint32_t v = (e >> (k * bits)) & ((1u << bits) - 1);
            uint32_t id = v & ((1u << (bits - 1)) - 1);
            bool range = v & (1u << (bits - 1));
            if (range && prev) {                        /* every node from prev+1 to id */
                for (uint32_t x = prev + 1; x <= id && w->nconn < MAX_CONN; x++) w->conn[w->nconn++] = (uint8_t)x;
            } else if (w->nconn < MAX_CONN) {
                w->conn[w->nconn++] = (uint8_t)id;
            }
            prev = id;
        }
    }
}

static bool codec_scan(int caddr)
{
    hda.caddr = caddr;
    hda.vendor = param(caddr, 0, P_VENDOR);
    uint32_t nodes = param(caddr, 0, P_NODE_COUNT);
    int start = (int)((nodes >> 16) & 0xff), count = (int)(nodes & 0xff);
    hda.afg = 0;
    for (int n = start; n < start + count; n++)
        if ((param(caddr, n, P_FG_TYPE) & 0xff) == 1) { hda.afg = (uint8_t)n; break; }
    if (!hda.afg) return false;
    verb(hda.afg, V_FUNC_RESET, 0);
    mdelay(10);
    verb(hda.afg, V_SET_POWER, 0);                      /* D0 */
    mdelay(10);
    uint32_t afg_pcm = param(caddr, hda.afg, P_PCM);
    uint32_t afg_out = param(caddr, hda.afg, P_AMP_OUT), afg_in = param(caddr, hda.afg, P_AMP_IN);
    nodes = param(caddr, hda.afg, P_NODE_COUNT);
    start = (int)((nodes >> 16) & 0xff);
    count = (int)(nodes & 0xff);
    memset(hda.w, 0, sizeof hda.w);
    for (int n = start; n < start + count && n < MAX_NID; n++) {
        struct widget *w = &hda.w[n];
        w->caps = param(caddr, n, P_WIDGET_CAPS);
        w->type = (uint8_t)((w->caps >> 20) & 0xf);
        if (w->caps & WC_CONN_LIST) read_conn_list(n, w);
        w->amp_out = (w->caps & WC_AMP_OVR) ? param(caddr, n, P_AMP_OUT) : afg_out;
        w->amp_in = (w->caps & WC_AMP_OVR) ? param(caddr, n, P_AMP_IN) : afg_in;
        if (w->type == W_PIN) {
            w->pincaps = param(caddr, n, P_PIN_CAPS);
            w->cfg = verb(n, V_GET_CFG_DEF, 0);
        }
        if (w->type == W_OUT) {
            w->pcm = param(caddr, n, P_PCM);
            if (!(w->pcm & 0xfff)) w->pcm = afg_pcm;
        }
        if (w->caps & WC_POWER) verb(n, V_SET_POWER, 0);
    }
    return true;
}

/* A path from nid down to a DAC, depth first: path[0] = nid ... path[len-1] = DAC. */
static int find_dac(int nid, uint8_t *path, int depth, int want_dac)
{
    if (nid <= 0 || nid >= MAX_NID || depth >= MAX_PATH) return 0;
    for (int i = 0; i < depth; i++) if (path[i] == nid) return 0;      /* a loop */
    struct widget *w = &hda.w[nid];
    path[depth] = (uint8_t)nid;
    if (w->type == W_OUT) {
        if (w->caps & WC_DIGITAL) return 0;
        return (!want_dac || nid == want_dac) ? depth + 1 : 0;
    }
    if (depth && w->type != W_MIXER && w->type != W_SELECTOR) return 0; /* through mixers and selectors only */
    for (int c = 0; c < w->nconn; c++) {
        int len = find_dac(w->conn[c], path, depth + 1, want_dac);
        if (len) return len;
    }
    return 0;
}

static int conn_index(const struct widget *w, int nid)
{
    for (int i = 0; i < w->nconn; i++) if (w->conn[i] == nid) return i;
    return -1;
}

/* Set an amplifier: output or input (index), both channels, gain in steps. */
static void set_amp(int nid, bool out, int index, int gain, bool mute)
{
    uint32_t p = (out ? 0x8000u : 0x4000u) | 0x3000u | ((uint32_t)index << 8) | (mute ? 0x80u : 0) | ((uint32_t)gain & 0x7f);
    verb(nid, V4_SET_AMP, p);
}
static int amp_steps(uint32_t caps) { return (int)((caps >> 8) & 0x7f); }
static int amp_0db(uint32_t caps) { return (int)(caps & 0x7f); }

static void add_vol(int nid, bool out)
{
    for (int i = 0; i < hda.nvol; i++) if (hda.vol_nid[i] == nid && hda.vol_out[i] == out) return;
    if (hda.nvol < (int)sizeof hda.vol_nid) { hda.vol_nid[hda.nvol] = (uint8_t)nid; hda.vol_out[hda.nvol++] = out; }
}

static void route(const uint8_t *path, int len)
{
    for (int i = 0; i < len; i++) {
        int nid = path[i];
        struct widget *w = &hda.w[nid];
        int next = i + 1 < len ? path[i + 1] : 0;
        int idx = next ? conn_index(w, next) : -1;
        if (idx >= 0 && w->type != W_MIXER && w->nconn > 1) verb(nid, V_SET_CONN_SEL, (uint32_t)idx);
        if (idx >= 0 && (w->caps & WC_IN_AMP)) set_amp(nid, false, idx, amp_0db(w->amp_in), false);
        if (w->caps & WC_OUT_AMP) {
            set_amp(nid, true, 0, amp_0db(w->amp_out), false);
            if (amp_steps(w->amp_out)) add_vol(nid, true);
        }
    }
}

static void pin_on(int nid)
{
    struct widget *w = &hda.w[nid];
    uint8_t ctl = PIN_OUT_EN;
    if (CFG_DEVICE(w->cfg) == DEV_HP_OUT && (w->pincaps & PC_HP_DRIVE)) ctl |= PIN_HP_EN;
    verb(nid, V_SET_PIN_CTL, ctl);
    if (w->pincaps & PC_EAPD) verb(nid, V_SET_EAPD, 0x02);   /* the external amplifier (Realtek: needed) */
}

static const char *dev_name(int d)
{
    switch (d) {
    case DEV_LINE_OUT: return "line out";
    case DEV_SPEAKER: return "speaker";
    case DEV_HP_OUT: return "headphones";
    default: return "output";
    }
}

/* Route every wired output pin to a DAC; true if at least one. */
static bool setup_outputs(void)
{
    hda.ndacs = 0;
    hda.nvol = 0;
    hda.outs[0] = 0;
    /* speakers and headphones first, then line outs; jacks and built-in ones */
    static const int order[] = { DEV_SPEAKER, DEV_HP_OUT, DEV_LINE_OUT };
    for (int o = 0; o < 3; o++) {
        for (int nid = 1; nid < MAX_NID; nid++) {
            struct widget *w = &hda.w[nid];
            if (w->type != W_PIN || !(w->pincaps & PC_OUT) || CFG_CONN(w->cfg) == 1) continue;
            if ((int)CFG_DEVICE(w->cfg) != order[o]) continue;
            uint8_t path[MAX_PATH];
            int len = 0;
            for (int d = 0; d < hda.ndacs && !len; d++) len = find_dac(nid, path, 0, hda.dacs[d]);  /* share a DAC */
            if (!len) len = find_dac(nid, path, 0, 0);
            if (!len) continue;
            int dac = path[len - 1];
            bool known = false;
            for (int d = 0; d < hda.ndacs; d++) known |= hda.dacs[d] == dac;
            if (!known) {
                if (hda.ndacs == MAX_DACS) continue;
                hda.dacs[hda.ndacs++] = (uint8_t)dac;
            }
            route(path, len);
            pin_on(nid);
            size_t n = strlen(hda.outs);
            snprintf(hda.outs + n, sizeof hda.outs - n, "%s%s (pin 0x%x, DAC 0x%x)", n ? ", " : "",
                     dev_name(CFG_DEVICE(w->cfg)), nid, dac);
        }
    }
    /* A codec with no configuration defaults (QEMU's hda-output says "line
     * out" for its pin, but a board may leave them at 0): any output pin. */
    if (!hda.ndacs) {
        for (int nid = 1; nid < MAX_NID && !hda.ndacs; nid++) {
            struct widget *w = &hda.w[nid];
            if (w->type != W_PIN || !(w->pincaps & PC_OUT)) continue;
            uint8_t path[MAX_PATH];
            int len = find_dac(nid, path, 0, 0);
            if (!len) continue;
            hda.dacs[hda.ndacs++] = path[len - 1];
            route(path, len);
            pin_on(nid);
            snprintf(hda.outs, sizeof hda.outs, "pin 0x%x, DAC 0x%x", nid, path[len - 1]);
        }
    }
    hda.rates = 0xfff;
    for (int d = 0; d < hda.ndacs; d++) hda.rates &= hda.w[hda.dacs[d]].pcm & 0xfff;
    if (!hda.rates) hda.rates = 1u << 6;                /* 48 kHz: every codec has it */
    return hda.ndacs > 0;
}

/* ---- stream format ------------------------------------------------------------------- */
static const struct { int rate; uint16_t fmt; int bit; } rate_tab[] = {
    {   8000, 0x0500, 0 }, {  11025, 0x4300, 1 }, {  16000, 0x0200, 2 }, {  22050, 0x4100, 3 },
    {  32000, 0x0a00, 4 }, {  44100, 0x4000, 5 }, {  48000, 0x0000, 6 }, {  88200, 0x4800, 7 },
    {  96000, 0x0800, 8 }, { 176400, 0x5800, 9 }, { 192000, 0x1800, 10 },
};

/* The supported rate nearest to `want`; its format word (16-bit stereo). */
static int pick_rate(int want, uint16_t *fmt)
{
    int best = -1;
    for (int i = 0; i < (int)(sizeof rate_tab / sizeof *rate_tab); i++) {
        if (!(hda.rates & (1u << rate_tab[i].bit))) continue;
        int d = rate_tab[i].rate - want, bd = best < 0 ? 0 : rate_tab[best].rate - want;
        if (d < 0) d = -d;
        if (bd < 0) bd = -bd;
        if (best < 0 || d < bd) best = i;
    }
    if (best < 0) best = 6;
    *fmt = (uint16_t)(rate_tab[best].fmt | 0x0010 | 0x0001);   /* 16 bits, 2 channels */
    return rate_tab[best].rate;
}

/* ---- the output stream --------------------------------------------------------------- */
static uint32_t hw_pos(void)
{
    uint32_t lpib = r32(sdreg(hda.sd, SD_LPIB));
    if (hda.use_lpib || !hda.posbuf) return lpib % RING_BYTES;
    uint32_t p = hda.posbuf[hda.sd * 2];
    /* a controller that does not update the position buffer: LPIB instead */
    if (p == 0 && lpib != 0) {
        if (!hda.pos_stuck_since) hda.pos_stuck_since = time_ms();
        else if (time_ms() - hda.pos_stuck_since > 50) { hda.use_lpib = true; kprintf("hda: position buffer idle, reading LPIB\n"); }
        return lpib % RING_BYTES;
    }
    hda.pos_stuck_since = 0;
    return p % RING_BYTES;
}

static void stream_stop(void)
{
    if (!hda.running) return;
    w8(sdreg(hda.sd, SD_CTL), (uint8_t)(r8(sdreg(hda.sd, SD_CTL)) & ~SD_CTL_RUN));
    wait8(sdreg(hda.sd, SD_CTL), SD_CTL_RUN, 0, 20);
    hda.running = false;
}

static void stream_start(void)
{
    int sd = hda.sd;
    stream_stop();
    w8(sdreg(sd, SD_CTL), SD_CTL_SRST);
    wait8(sdreg(sd, SD_CTL), SD_CTL_SRST, SD_CTL_SRST, 20);
    udelay(10);
    w8(sdreg(sd, SD_CTL), 0);
    wait8(sdreg(sd, SD_CTL), SD_CTL_SRST, 0, 20);
    w8(sdreg(sd, SD_STS), 0x1c);
    memset(hda.ring, 0, RING_BYTES);
    if (hda.posbuf) hda.posbuf[sd * 2] = 0;
    uint64_t b = (uint64_t)(uintptr_t)hda.bdl;
    w32(sdreg(sd, SD_BDPL), (uint32_t)b);
    w32(sdreg(sd, SD_BDPU), (uint32_t)(b >> 32));
    w32(sdreg(sd, SD_CBL), RING_BYTES);
    w16(sdreg(sd, SD_LVI), BDL_N - 1);
    w16(sdreg(sd, SD_FMT), hda.fmt);
    w8(sdreg(sd, SD_CTL) + 2, (uint8_t)(hda.tag << 4));    /* stream number; stripe 0, traffic priority 0 */
    for (int d = 0; d < hda.ndacs; d++) {
        verb(hda.dacs[d], V4_SET_FORMAT, hda.fmt);
        verb(hda.dacs[d], V_SET_STREAM, (uint32_t)hda.tag << 4);   /* channel 0 (and 1) */
    }
    hda.last_pos = 0;
    hda.played = hda.written = 0;
    hda.pos_stuck_since = 0;
    w8(sdreg(sd, SD_CTL), SD_CTL_RUN);
    hda.running = true;
}

/* Account for what the hardware has played since the last look, and zero it. */
static void update(void)
{
    if (!hda.running) return;
    uint32_t pos = hw_pos();
    uint32_t d = (pos + RING_BYTES - hda.last_pos) % RING_BYTES;
    if (!d) return;
    uint32_t a = hda.last_pos;
    for (uint32_t left = d; left; ) {
        uint32_t n = RING_BYTES - a < left ? RING_BYTES - a : left;
        memset(hda.ring + a, 0, n);
        a = (a + n) % RING_BYTES;
        left -= n;
    }
    hda.last_pos = pos;
    hda.played += d;
    if (hda.written < hda.played) hda.written = hda.played;     /* ran dry: carry on from here */
}

static uint32_t queued(void) { return (uint32_t)(hda.written - hda.played); }

#define LEAD    256u                    /* never write closer than this to the DMA engine */
#define GUARD   4096u                   /* never fill the ring completely */
static uint32_t space(void) { uint32_t q = queued(); return q + GUARD >= RING_BYTES ? 0 : RING_BYTES - GUARD - q; }

/* Copy 16-bit stereo frames into the ring at the write position. */
static void put_frames(const int16_t *src, uint32_t bytes)
{
    if (queued() < LEAD) hda.written = hda.played + LEAD;        /* just after the DMA engine */
    uint32_t at = (uint32_t)(hda.written % RING_BYTES);
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t left = bytes; left; ) {
        uint32_t n = RING_BYTES - at < left ? RING_BYTES - at : left;
        memcpy(hda.ring + at, s, n);
        s += n;
        at = (at + n) % RING_BYTES;
        left -= n;
    }
    hda.written += bytes;
}

/* The kernel thread: keeps the accounting going (and the played part silent)
 * while the stream runs; stops the stream a while after it has run dry. */
static int dsp_open_count;
static int hdad_main(void *arg)
{
    (void)arg;
    uint64_t dry_since = 0;
    bkl_release_all();                                  /* slock is all it needs */
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&slock);
        if (hda.running) {
            update();
            if (!queued() && !dsp_open_count) {
                if (!dry_since) dry_since = time_ms();
                else if (time_ms() - dry_since > 1000) { stream_stop(); dry_since = 0; }
            } else dry_since = 0;
            wakeup(&hda);
        }
        spin_unlock_irqrestore(&slock, fl);
        task_sleep_ms(hda.running ? 4 : 50);
    }
    return 0;
}

/* ---- volume ------------------------------------------------------------------------------ */
static void set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    uint64_t fl = spin_lock_irqsave(&slock);            /* verbs: the codec link */
    hda.volume = v;
    for (int i = 0; i < hda.nvol; i++) {
        struct widget *w = &hda.w[hda.vol_nid[i]];
        uint32_t caps = hda.vol_out[i] ? w->amp_out : w->amp_in;
        int steps = amp_steps(caps), zero = amp_0db(caps);
        int top = zero ? zero : steps;                  /* 100% = 0 dB, not the amplifier's maximum */
        int g = top * v / 100;
        set_amp(hda.vol_nid[i], hda.vol_out[i], 0, g, v == 0);
    }
    spin_unlock_irqrestore(&slock, fl);
}

/* ---- /dev/dsp ---------------------------------------------------------------------------- */
#define AFMT_U8      0x00000008
#define AFMT_S16_LE  0x00000010
#define AFMT_S32_LE  0x00001000
#define FRAG_BYTES   4096

static struct {
    int fmt, channels, rate;            /* what the program writes */
    uint8_t carry[8]; int ncarry;       /* a partial frame from the last write */
    /* rate conversion (when the codec cannot run at `rate`): linear
     * interpolation, `step` input frames per output frame in 16.16 */
    bool resample;
    uint32_t step, phase;
    int16_t prev[2];
} dsp;

static int in_frame_bytes(void) { return (dsp.fmt == AFMT_U8 ? 1 : dsp.fmt == AFMT_S32_LE ? 4 : 2) * dsp.channels; }

static int16_t sample_at(const uint8_t *p, int ch)
{
    switch (dsp.fmt) {
    case AFMT_U8: return (int16_t)(((int)p[ch] - 128) << 8);
    case AFMT_S32_LE: { int32_t v; memcpy(&v, p + 4 * ch, 4); return (int16_t)(v >> 16); }
    default: { int16_t v; memcpy(&v, p + 2 * ch, 2); return v; }
    }
}

/* `frames` input frames into 16-bit stereo; returns the output frame count
 * (at most `cap`; the caller passes few enough input frames). */
static int convert(const uint8_t *in, int frames, int16_t *out, int cap)
{
    int fb = in_frame_bytes(), n = 0;
    for (int i = 0; i < frames; i++, in += fb) {
        int16_t l = sample_at(in, 0), r = dsp.channels > 1 ? sample_at(in, 1) : l;
        if (!dsp.resample) { out[2 * n] = l; out[2 * n + 1] = r; n++; continue; }
        while (dsp.phase < 0x10000 && n < cap) {
            int32_t f = (int32_t)dsp.phase;
            out[2 * n] = (int16_t)(dsp.prev[0] + (((int32_t)l - dsp.prev[0]) * f >> 16));
            out[2 * n + 1] = (int16_t)(dsp.prev[1] + (((int32_t)r - dsp.prev[1]) * f >> 16));
            n++;
            dsp.phase += dsp.step;
        }
        dsp.phase -= 0x10000;
        dsp.prev[0] = l;
        dsp.prev[1] = r;
    }
    return n;
}

/* Output bytes (16-bit stereo at the hardware rate) as bytes of the program's format. */
static int to_in_bytes(uint64_t out)
{
    uint64_t frames = out / 4;
    if (dsp.resample) frames = frames * (uint64_t)dsp.rate / (uint64_t)hda.rate;
    return (int)(frames * (uint64_t)in_frame_bytes());
}

/* The hardware runs at the program's rate when the codec has it, else at
 * 48 kHz (or the nearest it has) with the driver converting. */
static void dsp_apply_rate(void)
{
    uint16_t fmt;
    int hw = pick_rate(dsp.rate, &fmt);
    if (hw != dsp.rate) hw = pick_rate(48000, &fmt);
    dsp.resample = hw != dsp.rate;
    dsp.step = (uint32_t)(((uint64_t)dsp.rate << 16) / (uint64_t)hw);
    dsp.phase = 0;
    dsp.prev[0] = dsp.prev[1] = 0;
    if (fmt != hda.fmt || hda.rate != hw) {
        uint64_t fl = spin_lock_irqsave(&slock);
        bool was = hda.running;
        stream_stop();
        hda.fmt = fmt;
        hda.rate = hw;
        if (was) stream_start();
        spin_unlock_irqrestore(&slock, fl);
    }
}

static int dsp_open(struct vnode *vn, int flags, struct file **out)
{
    if ((flags & O_ACCMODE) == O_RDONLY) return -ENODEV;   /* no capture yet */
    if (dsp_open_count) return -EBUSY;
    struct file *f = vfs_file_new(vn, flags);
    if (!f) return -ENOMEM;
    dsp_open_count++;
    dsp.fmt = AFMT_U8;                                  /* the OSS defaults */
    dsp.channels = 1;
    dsp.rate = 8000;
    dsp.ncarry = 0;
    dsp_apply_rate();
    *out = f;
    return 0;
}

static void dsp_release(struct file *f)
{
    (void)f;
    dsp_open_count--;
}

/* write: convert, wait for room (unless O_NONBLOCK), copy into the ring. */
/* write: convert, wait for room (unless O_NONBLOCK), copy into the ring.
 * Room means a fragment, or all of a smaller write: a nonblocking writer
 * gets EAGAIN instead of a trickle of tiny writes. */
static ssize_t dsp_write_locked(struct file *f, const void *buf, size_t len);

static ssize_t dsp_fwrite(struct file *f, const void *buf, size_t len)
{
    if (!hda.ready) return -EIO;
    kmutex_lock(&dsp_mutex);
    ssize_t r = dsp_write_locked(f, buf, len);
    kmutex_unlock(&dsp_mutex);
    return r;
}

static ssize_t dsp_write_locked(struct file *f, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    int fb = in_frame_bytes();
    static int16_t tmp[2 * 2048];
    const int cap = 2048;
    while (done < len) {
        uint64_t fl = spin_lock_irqsave(&slock);
        if (!hda.running) stream_start();
        update();
        uint32_t room = space();
        spin_unlock_irqrestore(&slock, fl);
        uint64_t want = (uint64_t)(len - done) / (uint64_t)fb * 4;          /* output bytes, about */
        if (dsp.resample) want = want * (uint64_t)hda.rate / (uint64_t)dsp.rate + 8;
        uint32_t need = want < FRAG_BYTES ? (uint32_t)want : FRAG_BYTES;
        if (need < 8 || done) need = 8;                 /* once started, fill what fits */
        if (room < need) {
            if (done) break;                            /* a short write; the caller writes the rest */
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            if (signal_pending()) return -ERESTARTSYS;
            kmutex_unlock(&dsp_mutex);                  /* ioctls (GETODELAY...) go on meanwhile */
            task_sleep_ms(4);
            kmutex_lock(&dsp_mutex);
            continue;
        }
        /* a partial frame left over from the last call first */
        if (dsp.ncarry) {
            while (dsp.ncarry < fb && done < len) dsp.carry[dsp.ncarry++] = p[done++];
            if (dsp.ncarry < fb) break;
            int n = convert(dsp.carry, 1, tmp, cap);
            fl = spin_lock_irqsave(&slock); put_frames(tmp, (uint32_t)n * 4); spin_unlock_irqrestore(&slock, fl);
            dsp.ncarry = 0;
            continue;
        }
        size_t frames = (len - done) / (size_t)fb;
        if (!frames) {                                  /* keep the tail for next time */
            while (done < len) dsp.carry[dsp.ncarry++] = p[done++];
            break;
        }
        uint64_t out_room = room / 4 < (uint32_t)cap ? room / 4 : (uint32_t)cap;   /* output frames */
        uint64_t max = dsp.resample ? out_room * (uint64_t)dsp.rate / (uint64_t)hda.rate : out_room;
        if (dsp.resample && max > 1) max -= 1;          /* rounding: never more output than room */
        if (!max) max = 1;
        if (frames > max) frames = max;
        int n = convert(p + done, (int)frames, tmp, cap);
        fl = spin_lock_irqsave(&slock);
        put_frames(tmp, (uint32_t)n * 4);
        spin_unlock_irqrestore(&slock, fl);
        done += frames * (size_t)fb;
    }
    return (ssize_t)done;
}

static int dsp_poll(struct file *f, int events)
{
    (void)f;
    poll_wait(&hda);                                    /* and the 4 ms poll deadline */
    uint64_t fl = spin_lock_irqsave(&slock);
    update();
    int r = (hda.ready && space() >= FRAG_BYTES) ? (events & (POLLOUT | POLLWRNORM)) : 0;
    spin_unlock_irqrestore(&slock, fl);
    return r;
}

static uint64_t dsp_poll_deadline(struct file *f) { (void)f; return __atomic_load_n(&hda.running, __ATOMIC_RELAXED) ? time_ms() + 4 : 0; }


static int put_int(void *arg, int v) { return copy_to_user(arg, &v, sizeof v) ? -EFAULT : 0; }
static int get_int(void *arg, int *v) { return copy_from_user(v, arg, sizeof *v) ? -EFAULT : 0; }

static int dsp_ioctl_locked(struct file *f, unsigned long req, void *arg);

static int dsp_ioctl(struct file *f, unsigned long req, void *arg)
{
    kmutex_lock(&dsp_mutex);
    int r = dsp_ioctl_locked(f, req, arg);
    kmutex_unlock(&dsp_mutex);
    return r;
}

static int dsp_ioctl_locked(struct file *f, unsigned long req, void *arg)
{
    int v, r;
    switch ((uint32_t)req) {
    case 0x5000:                                        /* SNDCTL_DSP_RESET */
    case 0x5008: {                                      /* SNDCTL_DSP_POST */
        uint64_t fl = spin_lock_irqsave(&slock);
        if (req == 0x5000 && hda.running) { update(); hda.written = hda.played; memset(hda.ring, 0, RING_BYTES); }
        spin_unlock_irqrestore(&slock, fl);
        dsp.ncarry = 0;
        return 0;
    }
    case 0x5001:                                        /* SNDCTL_DSP_SYNC: until played */
        for (;;) {
            uint64_t fl = spin_lock_irqsave(&slock);
            update();
            uint32_t q = hda.running ? queued() : 0;
            spin_unlock_irqrestore(&slock, fl);
            if (!q) return 0;
            if (signal_pending()) return -ERESTARTSYS;
            kmutex_unlock(&dsp_mutex);
            task_sleep_ms(4);
            kmutex_lock(&dsp_mutex);
        }
    case 0xC0045002:                                    /* SNDCTL_DSP_SPEED */
        if ((r = get_int(arg, &v))) return r;
        if (v <= 0) return -EINVAL;
        if (v < 4000) v = 4000;
        if (v > 192000) v = 192000;
        dsp.rate = v;
        dsp_apply_rate();
        return put_int(arg, dsp.rate);
    case 0x80045002:                                    /* SOUND_PCM_READ_RATE */
        return put_int(arg, dsp.rate);
    case 0xC0045003:                                    /* SNDCTL_DSP_STEREO */
        if ((r = get_int(arg, &v))) return r;
        dsp.channels = v ? 2 : 1;
        return put_int(arg, dsp.channels - 1);
    case 0xC0045004:                                    /* SNDCTL_DSP_GETBLKSIZE */
        return put_int(arg, to_in_bytes(FRAG_BYTES));
    case 0xC0045005:                                    /* SNDCTL_DSP_SETFMT */
        if ((r = get_int(arg, &v))) return r;
        if (v == AFMT_U8 || v == AFMT_S16_LE || v == AFMT_S32_LE) { dsp.fmt = v; dsp.ncarry = 0; }
        return put_int(arg, dsp.fmt);                   /* 0 (AFMT_QUERY) or unknown: the current one */
    case 0xC0045006:                                    /* SNDCTL_DSP_CHANNELS */
        if ((r = get_int(arg, &v))) return r;
        dsp.channels = v >= 2 ? 2 : 1;
        dsp.ncarry = 0;
        return put_int(arg, dsp.channels);
    case 0x80045006:                                    /* SOUND_PCM_READ_CHANNELS */
        return put_int(arg, dsp.channels);
    case 0xC0045009:                                    /* SNDCTL_DSP_SUBDIVIDE */
    case 0xC004500A:                                    /* SNDCTL_DSP_SETFRAGMENT: fixed fragments */
        return 0;
    case 0x8004500B:                                    /* SNDCTL_DSP_GETFMTS */
        return put_int(arg, AFMT_U8 | AFMT_S16_LE | AFMT_S32_LE);
    case 0x8010500C: {                                  /* SNDCTL_DSP_GETOSPACE */
        uint64_t fl = spin_lock_irqsave(&slock);
        update();
        uint32_t room = hda.running ? space() : RING_BYTES - GUARD;
        spin_unlock_irqrestore(&slock, fl);
        int fs = to_in_bytes(FRAG_BYTES), b = to_in_bytes(room);
        int info[4] = { b / fs, to_in_bytes(RING_BYTES - GUARD) / fs, fs, b };
        return copy_to_user(arg, info, sizeof info) ? -EFAULT : 0;
    }
    case 0x500E:                                        /* SNDCTL_DSP_NONBLOCK */
        f->flags |= O_NONBLOCK;
        return 0;
    case 0x8004500F:                                    /* SNDCTL_DSP_GETCAPS: trigger, realtime */
        return put_int(arg, 0x00001000 | 0x00000200);
    case 0x80045010:                                    /* SNDCTL_DSP_GETTRIGGER */
        return put_int(arg, 2);                         /* PCM_ENABLE_OUTPUT */
    case 0x40045010:                                    /* SNDCTL_DSP_SETTRIGGER */
        return 0;
    case 0x800C5012: {                                  /* SNDCTL_DSP_GETOPTR */
        uint64_t fl = spin_lock_irqsave(&slock);
        update();
        int ci[3] = { to_in_bytes(hda.played), (int)(hda.played / FRAG_BYTES), to_in_bytes(hda.last_pos) };
        spin_unlock_irqrestore(&slock, fl);
        return copy_to_user(arg, ci, sizeof ci) ? -EFAULT : 0;
    }
    case 0x80045017: {                                  /* SNDCTL_DSP_GETODELAY */
        uint64_t fl = spin_lock_irqsave(&slock);
        update();
        int d = hda.running ? to_in_bytes(queued()) : 0;
        spin_unlock_irqrestore(&slock, fl);
        return put_int(arg, d);
    }
    case 0x80044D76:                                    /* OSS_GETVERSION */
        return put_int(arg, 0x030802);
    default:
        return -ENOTTY;
    }
}

static const struct vnode_ops dsp_ops = {
    .open = dsp_open, .fwrite = dsp_fwrite, .poll = dsp_poll, .release = dsp_release,
    .fioctl = dsp_ioctl, .poll_deadline = dsp_poll_deadline, .nolock = true,
};

/* ---- /dev/mixer -------------------------------------------------------------------------- */
#define MIX_VOLUME 0
#define MIX_PCM    4
static int mixer_ioctl(struct file *f, unsigned long req, void *arg)
{
    (void)f;
    int v, r;
    switch ((uint32_t)req) {
    case 0x80044D00: case 0x80044D04:                   /* SOUND_MIXER_READ_VOLUME / _PCM */
        return put_int(arg, hda.volume | hda.volume << 8);
    case 0xC0044D00: case 0xC0044D04:                   /* SOUND_MIXER_WRITE_VOLUME / _PCM */
        if ((r = get_int(arg, &v))) return r;
        {
            int left = v & 0xff, right = (v >> 8) & 0xff;
            set_volume(left > right ? left : right);
        }
        return put_int(arg, hda.volume | hda.volume << 8);
    case 0x80044DFE:                                    /* SOUND_MIXER_READ_DEVMASK */
    case 0x80044DFB:                                    /* SOUND_MIXER_READ_STEREODEVS */
        return put_int(arg, (1 << MIX_VOLUME) | (1 << MIX_PCM));
    case 0x80044DFD: case 0x80044DFF:                   /* READ_RECMASK, READ_RECSRC */
    case 0x80044DFC:                                    /* SOUND_MIXER_READ_CAPS */
        return put_int(arg, 0);
    case 0x805C4D65: {                                  /* SOUND_MIXER_INFO */
        struct { char id[16], name[32]; int modify; int fill[10]; } mi;
        memset(&mi, 0, sizeof mi);
        strlcpy(mi.id, "HDA", sizeof mi.id);
        snprintf(mi.name, sizeof mi.name, "HD Audio %04x:%04x", hda.vendor >> 16, hda.vendor & 0xffff);
        return copy_to_user(arg, &mi, sizeof mi) ? -EFAULT : 0;
    }
    case 0x80044D76:                                    /* OSS_GETVERSION */
        return put_int(arg, 0x030802);
    default:
        return -ENOTTY;
    }
}

static const struct vnode_ops mixer_ops = { .fioctl = mixer_ioctl };

/* ---- probe ------------------------------------------------------------------------------- */
static const char *codec_vendor(uint32_t v)
{
    switch (v >> 16) {
    case 0x10ec: return "Realtek";
    case 0x8086: return "Intel";
    case 0x1af4: return "QEMU";
    case 0x14f1: return "Conexant";
    case 0x111d: case 0x8384: return "IDT/Sigmatel";
    case 0x1106: return "VIA";
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x11d4: return "Analog Devices";
    case 0x1013: return "Cirrus Logic";
    default: return "codec";
    }
}

static size_t gen_proc(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    size_t n = 0;
#define P(...) (n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__))
    if (!hda.present) { P("no HD Audio controller\n"); return n; }
    if (!hda.pci) { P("no HD Audio controller with a usable codec\n"); return n; }
    P("controller %04x:%04x at %02x:%02x.%x, %d in / %d out streams, %s commands\n", hda.pci->vendor_id,
      hda.pci->device_id, hda.pci->bus, hda.pci->device, hda.pci->function, hda.iss, hda.oss,
      hda.immediate ? "immediate" : "CORB/RIRB");
    if (!hda.ready) { P("no usable codec\n"); return n; }
    if ((hda.vendor >> 16) == 0x10ec) P("codec %d: Realtek ALC%x, audio function group 0x%x\n", hda.caddr, hda.vendor & 0xffff, hda.afg);
    else P("codec %d: %s %04x:%04x, audio function group 0x%x\n", hda.caddr, codec_vendor(hda.vendor),
           hda.vendor >> 16, hda.vendor & 0xffff, hda.afg);
    P("outputs: %s\n", hda.outs);
    P("stream: %d Hz 16-bit stereo, %s, played %lu bytes, queued %u\n", hda.rate,
      hda.running ? "running" : "stopped", hda.played, hda.running ? queued() : 0);
    P("volume: %d%%\n", hda.volume);
#undef P
    return n;
}

/* Bring up one controller and its first codec with an analog output. */
static bool try_controller(struct pci_device *d)
{
    memset(&hda, 0, sizeof hda);
    hda.present = true;
    hda.pci = d;
    struct pci_bar *b = &d->bars[0];
    if ((b->kind != PCI_BAR_MEM32 && b->kind != PCI_BAR_MEM64) || !b->base) { kprintf("hda: no register window\n"); return false; }
    if (!vmm_set_uncached(b->base, b->size ? b->size : 0x4000)) { kprintf("hda: cannot map registers\n"); return false; }
    hda.mm = (volatile uint8_t *)(uintptr_t)b->base;
    pci_enable(d, false, true, true);
    if (d->vendor_id == 0x8086)                         /* Intel: traffic class 0 (TCSEL), as Linux does */
        pci_write32(d->bus, d->device, d->function, 0x44, pci_read32(d->bus, d->device, d->function, 0x44) & ~0x7u);
    uint16_t gcap = r16(GCAP);
    hda.iss = (gcap >> 8) & 0xf;
    hda.oss = (gcap >> 12) & 0xf;
    if (!hda.oss) { kprintf("hda: %04x:%04x has no output streams\n", d->vendor_id, d->device_id); return false; }
    if (!controller_reset()) { kprintf("hda: %04x:%04x does not leave reset\n", d->vendor_id, d->device_id); return false; }
    uint16_t codecs = r16(STATESTS);
    if (!rings_start()) { kprintf("hda: command rings do not start; immediate commands\n"); hda.immediate = true; }
    hda.posbuf = dma_alloc(4096);
    if (hda.posbuf) {
        uint64_t p = (uint64_t)(uintptr_t)hda.posbuf;
        w32(DPUBASE, (uint32_t)(p >> 32));
        w32(DPLBASE, (uint32_t)p | 1);
    }
    kprintf("hda: controller %04x:%04x, HDA %u.%u, %d in / %d out streams, codecs 0x%x\n", d->vendor_id,
            d->device_id, r8(VMAJ), r8(VMIN), hda.iss, hda.oss, codecs);
    /* the first codec with an audio function group that drives an analog
     * output (HDMI codecs, digital only, are passed over) */
    for (int c = 0; c < 15 && !hda.ready; c++) {
        if (!(codecs & (1u << c))) continue;
        if (!codec_scan(c)) continue;
        if ((hda.vendor >> 16) == 0x10ec)
            kprintf("hda: codec %d: Realtek ALC%x (10ec:%04x)\n", c, hda.vendor & 0xffff, hda.vendor & 0xffff);
        else
            kprintf("hda: codec %d: %s %04x:%04x\n", c, codec_vendor(hda.vendor), hda.vendor >> 16, hda.vendor & 0xffff);
        if (setup_outputs()) hda.ready = true;
        else kprintf("hda: codec %d: no analog output path\n", c);
    }
    return hda.ready;
}

void hda_init(void)
{
    bool found = false;
    for (size_t i = 0; i < pci_device_count() && !hda.ready; i++) {
        struct pci_device *c = pci_device_at(i);
        if (c->class_code != 0x04 || c->subclass != 0x03) continue;   /* HD Audio, any vendor */
        found = true;
        if (!try_controller(c)) hda.ready = false;
    }
    if (!hda.ready) { hda.present = found; return; }
    hda.ring = dma_alloc(RING_BYTES);
    hda.bdl = dma_alloc(BDL_N * sizeof(struct bdl_entry));
    if (!hda.ring || !hda.bdl) { hda.ready = false; return; }
    for (int i = 0; i < BDL_N; i++) {
        hda.bdl[i].addr = (uint64_t)(uintptr_t)(hda.ring + i * (RING_BYTES / BDL_N));
        hda.bdl[i].len = RING_BYTES / BDL_N;
        hda.bdl[i].ioc = 0;
    }
    hda.sd = hda.iss;                                   /* the first output stream */
    hda.tag = 1;
    hda.rate = pick_rate(48000, &hda.fmt);
    set_volume(100);
    kprintf("hda: outputs: %s; rates mask 0x%x\n", hda.outs, hda.rates);
    devfs_register("dsp", 0666, (14u << 8) | 3u, &dsp_ops, NULL);
    devfs_register("mixer", 0666, (14u << 8) | 0u, &mixer_ops, NULL);
    task_create("hdad", hdad_main, NULL);
}

size_t hda_proc(char *buf, size_t cap) { return gen_proc(buf, cap, NULL); }
