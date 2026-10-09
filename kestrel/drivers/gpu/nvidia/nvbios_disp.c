/* =============================================================================
 *  nvbios_disp.c -- the VBIOS tables a modeset needs (see nvbios.h)
 *
 *    - the display ("U") table: per output, the IED scripts the display
 *      supervisor runs (OffInt / OnInt) and the clock-dependent scripts
 *    - the DP ("d") table: link training scripts, per-rate link config,
 *      and the drive current / pre-emphasis / TX power for each level
 *    - the PLL limits ("C") table: VPLL constraints and its register
 *
 *  Each function follows the nouveau function of the same name in
 *  nvkm/subdev/bios/{disp,dp,pll}.c (Copyright 2012 Red Hat Inc., and
 *  2005-2009 Erik Waling, Stephane Marchesin, Stuart Bennett; MIT licence),
 *  including how one lookup's ver/hdr/cnt/len feed the next.
 * ============================================================================= */
#include "nvbios.h"

/* ---- display table ---------------------------------------------------------- */
static uint32_t disp_table(const struct nvbios *b, uint8_t *ver, uint8_t *hdr, uint8_t *cnt, uint8_t *len, uint8_t *sub)
{
    uint8_t v; uint16_t off, l;
    if (!nvbios_bit_entry(b, 'U', &v, &off, &l) || v != 1) return 0;
    uint32_t data = nvbios_rd16(b, off);
    if (!data) return 0;
    *ver = nvbios_rd08(b, data);
    if (*ver < 0x20 || *ver > 0x22) return 0;
    *hdr = nvbios_rd08(b, data + 1);
    *len = nvbios_rd08(b, data + 2);
    *cnt = nvbios_rd08(b, data + 3);
    *sub = nvbios_rd08(b, data + 4);
    return data;
}

/* nvbios_outp_entry/parse: entry idx of the display table. On return
 * o->hdr is the sub-table header size, o->cnt its config count, o->len 6. */
static uint32_t outp_parse(const struct nvbios *b, uint8_t idx, struct nvbios_outp *o)
{
    uint8_t hdr, cnt, len, sub;
    uint32_t t = disp_table(b, &o->ver, &hdr, &cnt, &len, &sub);
    if (!t || idx >= cnt) { o->ver = 0; return 0; }
    uint32_t e = t + hdr + (uint32_t)idx * len;
    if (len < 2) return 0;
    uint32_t data = nvbios_rd16(b, e);
    if (!data) return 0;
    o->hdr = sub;
    o->cnt = nvbios_rd08(b, data + 5);
    o->len = 6;
    if (o->hdr < 0x0a) return 0;
    o->type = nvbios_rd16(b, data);
    o->mask = nvbios_rd32(b, data + 2);
    if (o->ver <= 0x20) o->mask |= 0x00c0;             /* match any link */
    o->script[0] = nvbios_rd16(b, data + 6);
    o->script[1] = nvbios_rd16(b, data + 8);
    o->script[2] = o->hdr >= 0x0c ? nvbios_rd16(b, data + 0x0a) : 0;
    return data;
}

uint32_t nvbios_outp_match(const struct nvbios *b, uint16_t type, uint16_t mask, struct nvbios_outp *o)
{
    for (int idx = 0; idx < 256; idx++) {
        uint32_t data = outp_parse(b, (uint8_t)idx, o);
        if (!data && !o->ver) break;
        if (data && o->type == type && (o->mask & mask) == mask) { o->data = data; return data; }
    }
    o->data = 0;
    return 0;
}

uint32_t nvbios_ocfg_match(const struct nvbios *b, const struct nvbios_outp *o, uint8_t proto, uint8_t flags,
                           struct nvbios_ocfg *c)
{
    for (int idx = 0; idx < o->cnt; idx++) {
        uint32_t data = o->data + o->hdr + (uint32_t)idx * o->len;
        c->proto     = nvbios_rd08(b, data);
        c->flags     = nvbios_rd08(b, data + 1);       /* nouveau: (u8)rd16(data + 1) */
        c->clkcmp[0] = nvbios_rd16(b, data + 2);
        c->clkcmp[1] = nvbios_rd16(b, data + 4);
        if ((c->proto == proto || c->proto == 0xff) && c->flags == flags) return data;
    }
    return 0;
}

uint16_t nvbios_oclk_match(const struct nvbios *b, uint16_t cmp, uint32_t khz)
{
    for (int guard = 0; cmp && guard < 64; guard++, cmp = (uint16_t)(cmp + 4))
        if (khz / 10 >= nvbios_rd16(b, cmp)) return nvbios_rd16(b, cmp + 2);
    return 0;
}

/* ---- DP table ------------------------------------------------------------------- */
static uint32_t dp_table(const struct nvbios *b, uint8_t *ver, uint8_t *hdr, uint8_t *cnt, uint8_t *len)
{
    uint8_t v; uint16_t off, l;
    if (!nvbios_bit_entry(b, 'd', &v, &off, &l) || v != 1 || l < 2) return 0;
    uint32_t data = nvbios_rd16(b, off);
    if (!data) return 0;
    *ver = nvbios_rd08(b, data);
    switch (*ver) {
    case 0x20: case 0x21: case 0x30: case 0x40: case 0x41: case 0x42:
        *hdr = nvbios_rd08(b, data + 1);
        *len = nvbios_rd08(b, data + 2);
        *cnt = nvbios_rd08(b, data + 3);
        return data;
    default:
        return 0;
    }
}

uint8_t nvbios_dp_version(const struct nvbios *b)
{
    uint8_t ver = 0, hdr, cnt, len;
    return dp_table(b, &ver, &hdr, &cnt, &len) ? ver : 0;
}

static uint32_t dpout_parse(const struct nvbios *b, uint8_t idx, struct nvbios_dpout *o)
{
    uint8_t hdr, cnt, len;
    uint32_t t = dp_table(b, &o->ver, &hdr, &cnt, &len);
    if (!t || idx >= cnt) { o->ver = 0; return 0; }
    uint32_t data = nvbios_rd16(b, t + hdr + (uint32_t)idx * len);
    if (!data) return 0;
    o->hdr = nvbios_rd08(b, t + 4);
    switch (o->ver) {
    case 0x20: case 0x21: case 0x30:
        o->len = nvbios_rd08(b, t + 5);
        o->cnt = nvbios_rd08(b, data + 4);
        break;
    default:
        o->len = o->cnt = 0;
        break;
    }
    o->type = nvbios_rd16(b, data);
    o->mask = nvbios_rd16(b, data + 2);
    for (int i = 0; i < 5; i++) o->script[i] = 0;
    o->lnkcmp = 0;
    switch (o->ver) {
    case 0x20:
        o->mask |= 0x00c0;
        /* fall through */
    case 0x21: case 0x30:
        o->flags = nvbios_rd08(b, data + 5);
        o->script[0] = nvbios_rd16(b, data + 6);
        o->script[1] = nvbios_rd16(b, data + 8);
        if (o->len >= 0x0c) o->lnkcmp = nvbios_rd16(b, data + 0x0a);
        if (o->len >= 0x0f) { o->script[2] = nvbios_rd16(b, data + 0x0c); o->script[3] = nvbios_rd16(b, data + 0x0e); }
        if (o->len >= 0x11) o->script[4] = nvbios_rd16(b, data + 0x10);
        break;
    case 0x40: case 0x41: case 0x42:
        o->flags     = nvbios_rd08(b, data + 0x04);
        o->script[0] = nvbios_rd16(b, data + 0x05);
        o->script[1] = nvbios_rd16(b, data + 0x07);
        o->lnkcmp    = nvbios_rd16(b, data + 0x09);
        o->script[2] = nvbios_rd16(b, data + 0x0b);
        o->script[3] = nvbios_rd16(b, data + 0x0d);
        o->script[4] = nvbios_rd16(b, data + 0x0f);
        break;
    default:
        return 0;
    }
    return data;
}

uint32_t nvbios_dpout_match(const struct nvbios *b, uint16_t type, uint16_t mask, struct nvbios_dpout *o)
{
    for (int idx = 0; idx < 256; idx++) {
        uint32_t data = dpout_parse(b, (uint8_t)idx, o);
        if (!data && !o->ver) break;
        if (data && o->type == type && (o->mask & mask) == mask) { o->data = data; return data; }
    }
    o->data = 0;
    return 0;
}

uint32_t nvbios_dpcfg_match(const struct nvbios *b, const struct nvbios_dpout *o, uint8_t pc, uint8_t vs, uint8_t pe,
                            struct nvbios_dpcfg *c)
{
    static const uint8_t vsoff[] = { 0, 4, 7, 9 };
    c->pc = c->dc = c->pe = c->tx_pu = 0;
    if (o->ver < 0x30 || !o->data) return 0;          /* the 2.x search form is pre-Fermi only */
    uint32_t idx = (uint32_t)pc * 10 + vsoff[vs & 3] + pe;
    if (o->ver >= 0x40 && o->ver <= 0x41 && o->hdr >= 0x12) idx += nvbios_rd08(b, o->data + 0x11) * 40u;
    else if (o->ver >= 0x42) idx += nvbios_rd08(b, o->data + 0x11) * 10u;

    /* nvbios_dpcfg_entry */
    uint32_t base = o->data, hdr = o->hdr, len = o->len, cnt = o->cnt;
    if (o->ver >= 0x40) {
        uint8_t v, h, n, l;
        base = dp_table(b, &v, &h, &n, &l);
        if (!base) return 0;
        hdr = h + (uint32_t)l * n;
        len = nvbios_rd08(b, base + 6);
        cnt = (uint32_t)nvbios_rd08(b, base + 7) * nvbios_rd08(b, base + 5);
    }
    if (idx >= cnt) return 0;
    uint32_t data = base + hdr + idx * len;
    switch (o->ver) {
    case 0x30: case 0x40: case 0x41:
        c->pc = nvbios_rd08(b, data); c->dc = nvbios_rd08(b, data + 1);
        c->pe = nvbios_rd08(b, data + 2); c->tx_pu = nvbios_rd08(b, data + 3);
        break;
    case 0x42:
        c->dc = nvbios_rd08(b, data); c->pe = nvbios_rd08(b, data + 1); c->tx_pu = nvbios_rd08(b, data + 2);
        break;
    default:
        return 0;
    }
    return data;
}

/* ---- PLL limits table --------------------------------------------------------- */
static uint32_t pll_limits_table(const struct nvbios *b, uint8_t *ver, uint8_t *hdr, uint8_t *cnt, uint8_t *len)
{
    uint8_t v; uint16_t off, l;
    uint32_t data = 0;
    *ver = 0;
    if (!nvbios_bit_entry(b, 'C', &v, &off, &l)) return 0;
    if (v == 1 && l >= 10) data = nvbios_rd16(b, off + 8);
    if (v == 2 && l >= 4)  data = nvbios_rd32(b, off);
    if (!data) return 0;
    *ver = nvbios_rd08(b, data);
    *hdr = nvbios_rd08(b, data + 1);
    *len = nvbios_rd08(b, data + 2);
    *cnt = nvbios_rd08(b, data + 3);
    return data;
}

bool nvbios_pll_parse(const struct nvbios *b, uint8_t type, struct nvbios_pll *p)
{
    uint8_t ver, hdr = 0, cnt = 0, len = 0;
    uint32_t data = pll_limits_table(b, &ver, &hdr, &cnt, &len), reg = 0, e = 0;
    for (uint8_t *q = (uint8_t *)p; q < (uint8_t *)(p + 1); q++) *q = 0;
    p->ver = ver;
    if (!data || ver < 0x30) return false;            /* older layouts: pre-Fermi boards only */
    data += hdr;
    for (int i = 0; i < cnt; i++, data += len)
        if (nvbios_rd08(b, data) == type) { e = data; reg = ver < 0x50 ? nvbios_rd32(b, data + 3) : 0; break; }
    if (!e) return false;
    p->type = type;
    p->reg = reg;
    p->entry = e;
    switch (ver) {
    case 0x30:
        data = nvbios_rd16(b, e + 1);
        p->vco1.min_freq = nvbios_rd16(b, data + 0) * 1000u;
        p->vco1.max_freq = nvbios_rd16(b, data + 2) * 1000u;
        p->vco1.min_inputfreq = nvbios_rd16(b, data + 8) * 1000u;
        p->vco1.max_inputfreq = nvbios_rd16(b, data + 12) * 1000u;
        p->vco1.min_n = nvbios_rd08(b, data + 16);
        p->vco1.max_n = nvbios_rd08(b, data + 17);
        p->vco1.min_m = nvbios_rd08(b, data + 18);
        p->vco1.max_m = nvbios_rd08(b, data + 19);
        p->max_p = nvbios_rd08(b, data + 25);
        p->refclk = nvbios_rd32(b, data + 28);
        break;
    case 0x40:
        p->refclk = nvbios_rd16(b, e + 9) * 1000u;
        data = nvbios_rd16(b, e + 1);
        p->vco1.min_freq = nvbios_rd16(b, data + 0) * 1000u;
        p->vco1.max_freq = nvbios_rd16(b, data + 2) * 1000u;
        p->vco1.min_inputfreq = nvbios_rd16(b, data + 4) * 1000u;
        p->vco1.max_inputfreq = nvbios_rd16(b, data + 6) * 1000u;
        p->vco1.min_m = nvbios_rd08(b, data + 8);
        p->vco1.max_m = nvbios_rd08(b, data + 9);
        p->vco1.min_n = nvbios_rd08(b, data + 10);
        p->vco1.max_n = nvbios_rd08(b, data + 11);
        p->min_p = nvbios_rd08(b, data + 12);
        p->max_p = nvbios_rd08(b, data + 13);
        break;
    case 0x50:
        p->refclk = nvbios_rd16(b, e + 1) * 1000u;
        p->vco1.min_freq = nvbios_rd16(b, e + 5) * 1000u;
        p->vco1.max_freq = nvbios_rd16(b, e + 7) * 1000u;
        p->vco1.min_inputfreq = nvbios_rd16(b, e + 9) * 1000u;
        p->vco1.max_inputfreq = nvbios_rd16(b, e + 11) * 1000u;
        p->vco1.min_m = nvbios_rd08(b, e + 13);
        p->vco1.max_m = nvbios_rd08(b, e + 14);
        p->vco1.min_n = nvbios_rd08(b, e + 15);
        p->vco1.max_n = nvbios_rd08(b, e + 16);
        p->min_p = nvbios_rd08(b, e + 17);
        p->max_p = nvbios_rd08(b, e + 18);
        break;
    default:
        return false;
    }
    return true;
}

/* nouveau clk/pllgt215.c: gt215_pll_calc(), with the fractional N path. */
int nvbios_pll_calc(const struct nvbios_pll *p, uint32_t freq, int *pN, int *pfN, int *pM, int *pP)
{
    uint32_t best_err = ~0u;
    int P;
    if (!freq || !p->refclk || !p->vco1.max_inputfreq || !p->vco1.min_inputfreq) return -1;
    P = (int)(p->vco1.max_freq / freq);
    if (P > p->max_p) P = p->max_p;
    if (P < p->min_p) P = p->min_p;
    if (P < 1) P = 1;
    int lM = (int)((p->refclk + p->vco1.max_inputfreq) / p->vco1.max_inputfreq);
    if (lM < p->vco1.min_m) lM = p->vco1.min_m;
    int hM = (int)((p->refclk + p->vco1.min_inputfreq) / p->vco1.min_inputfreq);
    if (hM > p->vco1.max_m) hM = p->vco1.max_m;
    if (lM > hM) lM = hM;
    if (lM < 1) lM = 1;
    for (int M = lM; M <= hM; M++) {
        uint32_t tmp = freq * (uint32_t)P * (uint32_t)M;
        int N = (int)(tmp / p->refclk);
        int fN = (int)(tmp % p->refclk);
        if (!pfN) {
            if ((uint32_t)fN >= p->refclk / 2) N++;
        } else {
            if ((uint32_t)fN < p->refclk / 2) N--;
            fN = (int)(tmp - (uint32_t)N * p->refclk);
        }
        if (N < p->vco1.min_n) continue;
        if (N > p->vco1.max_n) break;
        uint32_t got = p->refclk * (uint32_t)N / (uint32_t)M / (uint32_t)P;
        uint32_t err = got > freq ? got - freq : freq - got;
        if (err < best_err) { best_err = err; *pN = N; *pM = M; }
        if (pfN) {
            *pfN = (int)((((uint32_t)fN << 13) + p->refclk / 2) / p->refclk);
            *pfN = (*pfN - 4096) & 0xffff;
            *pP = P;
            return (int)freq;
        }
    }
    if (best_err == ~0u) return -1;
    *pP = P;
    return (int)(p->refclk * (uint32_t)*pN / (uint32_t)*pM / (uint32_t)P);
}
