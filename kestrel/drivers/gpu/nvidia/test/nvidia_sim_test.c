/* Host test for nvidia.c against a simulated GP106 BAR0.
 *
 * The simulation implements the registers the stage-1 probe touches, with
 * the semantics nouveau relies on:
 *   0x000000 PMC_BOOT_0, 0x100ce0 VRAM size, 0x021c04 display fuse
 *   0x619f04 + 0x001700 + 0x700000  PRAMIN window onto VRAM holding the VBIOS
 *   0x088050 + 0x300000             PROM, readable only with the shadow bit off
 *   0x00d950..0x00d97c + ch*0x50    GM200 AUX channels; a DisplayPort monitor
 *                                   (DPCD + EDID) sits on AUX channel 1
 *   0x612004, 0x022448, 0x640000+   display capabilities and armed state:
 *                                   head 0 drives SOR 1 (DP-A), 2560x1440
 *   0x612308 + or*0x100              SOR routing
 * It checks what the probe reports, and that every register it changed
 * temporarily (0x001700, 0x088050) is restored.
 *
 *   make test-nvidia
 */
#include "nvidia.h"
#include "synthrom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---- simulated hardware ---------------------------------------------------- */
#define VRAM_SIZE   (8u << 20)
#define BIOS_VRAM   0x00500000u             /* where "the VBIOS" left its image, 64 KiB aligned */
static uint8_t  vram[VRAM_SIZE];
static uint32_t r1700 = 0x0040, r88050 = 0x00000001, regs_written;
static uint32_t aux_ctrl[8], aux_addr[8], aux_stat[8], aux_out[8][4], aux_in[8][4], aux_pad[8][2], aux_dpcd_auto[8];
static uint8_t  dpcd[0x300], edid[256];
static uint32_t edid_ptr;
static const int SINK_CH = 1;
static uint32_t prom_reads, prom_reads_shadowed;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void aux_transaction(int ch)
{
    uint32_t c = aux_ctrl[ch], type = (c >> 12) & 0xf, size = (c & 0x100) ? 0 : (c & 0xff) + 1;
    uint8_t out[16], in[16] = { 0 };
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(aux_out[ch][i / 4] >> (8 * (i % 4)));
    uint32_t reply = 0, got = 0;
    if (ch != SINK_CH) { aux_stat[ch] = 0x00000100; return; }       /* no reply: timeout */
    switch (type & ~4u) {
    case 0x9:                                           /* native read */
        for (uint32_t i = 0; i < size; i++) in[i] = dpcd[(aux_addr[ch] + i) % sizeof dpcd];
        got = size; break;
    case 0x8: got = 0; break;                           /* native write: accept */
    case 0x0:                                           /* I2C write: EDID offset */
        if (aux_addr[ch] == 0x50 && size) edid_ptr = out[0]; else reply = 4;
        break;
    case 0x1:                                           /* I2C read */
        if (aux_addr[ch] != 0x50) { reply = 4; break; }
        for (uint32_t i = 0; i < size; i++) in[i] = edid[(edid_ptr + i) % 256];
        edid_ptr += size; got = size; break;
    }
    for (int i = 0; i < 4; i++) aux_in[ch][i] = le32(in + 4 * i);
    aux_stat[ch] = 0x10000000 | reply << 16 | got;
}

uint32_t nv_sim_rd32(uint32_t r)
{
    if (r == 0x000000) return 0x136000a1;               /* GP106 rev a1 */
    if (r == 0x100ce0) return (5u << 4) | 10;           /* 5 << (10 + 20) = 5 GiB */
    if (r == 0x021c04) return 0;
    if (r == 0x619f04) return ((BIOS_VRAM >> 8) & 0xffffff00) | 0x8 | 0x1;
    if (r == 0x001700) return r1700;
    if (r == 0x088050) return r88050;
    if (r >= 0x700000 && r < 0x800000) return le32(&vram[((uint64_t)r1700 << 16) + (r - 0x700000)]);
    if (r >= 0x300000 && r < 0x400000) {
        prom_reads++;
        if (r88050 & 1) { prom_reads_shadowed++; return 0xffffffff; }
        uint32_t o = r - 0x300000;
        return o + 4 <= sizeof rom ? le32(&rom[o]) : 0;
    }
    if (r >= 0x00d930 && r < 0x00d930 + 8 * 0x50) {
        int ch = (int)((r - 0x00d930) / 0x50); uint32_t o = (r - 0x00d930) % 0x50 + 0xd930;
        if (o >= 0xd940 && o < 0xd950) return aux_in[ch][(o - 0xd940) / 4];
        if (o == 0xd950) return aux_addr[ch];
        if (o == 0xd954) return aux_ctrl[ch];
        if (o == 0xd958) return (ch == SINK_CH ? 0x10000000 : 0) | (aux_stat[ch] & ~0x10000000u);
        if (o == 0xd968) return aux_dpcd_auto[ch];
        if (o == 0xd970) return aux_pad[ch][0];
        if (o == 0xd97c) return aux_pad[ch][1];
        return 0;
    }
    if (r == 0x612004) return 0x0f | 0x0f00;            /* heads 0-3, SORs 0-3 */
    if (r == 0x022448) return 4;
    if (r == 0x640200 + 1 * 0x20) return 0x00000801;   /* SOR 1: DP-A, head 0 */
    if (r >= 0x640400 && r < 0x640700) {             /* head 0 methods 0x404..0x4ff */
        switch (r - 0x640000) {
        case 0x404: return 5 << 6;                      /* 24 bpp */
        case 0x414: return 1481 << 16 | 2720;
        case 0x418: return 5 << 16 | 32;
        case 0x41c: return 38 << 16 | 112;
        case 0x420: return 1478 << 16 | 2672;
        case 0x450: return 241500000;
        case 0x460: return 0x00100000;                  /* surface at VRAM 0x10000000 */
        case 0x4b8: return 1440 << 16 | 2560;
        }
        return 0;
    }
    if (r == 0x612308 + 1 * 0x100) return 0x02;        /* DCB "or" bit 1 -> SOR 1, link A */
    return 0;
}

void nv_sim_wr32(uint32_t r, uint32_t v)
{
    regs_written++;
    if (r == 0x001700) { r1700 = v; return; }
    if (r == 0x088050) { r88050 = v; return; }
    if (r >= 0x00d930 && r < 0x00d930 + 8 * 0x50) {
        int ch = (int)((r - 0x00d930) / 0x50); uint32_t o = (r - 0x00d930) % 0x50 + 0xd930;
        if (o >= 0xd930 && o < 0xd940) { aux_out[ch][(o - 0xd930) / 4] = v; return; }
        if (o == 0xd950) { aux_addr[ch] = v; return; }
        if (o == 0xd958) return;                        /* status: read-to-clear in hardware */
        if (o == 0xd968) { aux_dpcd_auto[ch] = v; return; }
        if (o == 0xd970) { aux_pad[ch][0] = v; return; }
        if (o == 0xd97c) { aux_pad[ch][1] = v; return; }
        if (o == 0xd954) {
            /* "unksel" handshake: a 0x00100000 request is answered with 0x01000000 */
            uint32_t c = v & ~0x07000000u;
            if (v & 0x00100000) c |= 0x01000000;
            if (v & 0x80000000) { aux_ctrl[ch] = c & ~0x80000000u; return; }    /* reset */
            if (v & 0x00010000) { aux_ctrl[ch] = c & ~0x00010000u; aux_transaction(ch); return; }
            aux_ctrl[ch] = c;
            return;
        }
    }
}

/* ---- platform ----------------------------------------------------------------- */
static void *map_mmio(uint64_t phys, size_t size) { (void)phys; (void)size; static uint8_t dummy; return &dummy; }
static void delay_us(uint32_t us) { (void)us; }
static void log_putc(char c) { putchar(c); }
static void *alloc(size_t n) { return calloc(1, n); }
static void release(void *p, size_t n) { (void)n; free(p); }
static const struct nv_platform plat = { map_mmio, delay_us, log_putc, alloc, release, NULL };

int main(void)
{
    build();
    memcpy(&vram[BIOS_VRAM], rom, sizeof rom);
    /* DPCD 1.4 sink: 4 lanes, 8.1 Gbps, enhanced framing; firmware trained 4 x 5.4 */
    dpcd[0] = 0x14; dpcd[1] = 0x1e; dpcd[2] = 0x84; dpcd[0x100] = 0x14; dpcd[0x101] = 0x84;
    dpcd[0x200] = 0x41; dpcd[0x202] = 0x77; dpcd[0x203] = 0x77; dpcd[0x204] = 0x01;
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memcpy(edid, hdr, 8);
    edid[8] = 0x10; edid[9] = 0xac;                    /* "DEL" */
    edid[10] = 0x34; edid[11] = 0x12;
    edid[54] = 0x56; edid[55] = 0x5e;                  /* 241.5 MHz */
    edid[56] = 0x00; edid[58] = 0xa0;                  /* 2560 */
    edid[59] = 0xa0; edid[61] = 0x50;                  /* 1440 */
    edid[126] = 1;                                     /* one extension block */
    for (int i = 128; i < 256; i++) edid[i] = (uint8_t)i;

    struct nv_device *d = calloc(1, sizeof *d);
    d->vendor_id = 0x10de; d->device_id = 0x1c30; d->bar0 = 0xf6000000; d->bar1 = 0xe0000000; d->bar1_size = 256u << 20;
    int rc = nv_probe(d, &plat);
    CHECK(rc == NV_OK);
    CHECK(d->chipset == 0x136 && !strcmp(d->chip_name, "GP106") && !strcmp(d->family, "Pascal"));
    CHECK(d->vram_bytes == 5ull << 30);
    CHECK(d->bios_ok && !strcmp(d->bios_source, "PRAMIN") && d->bios_size == sizeof rom);
    CHECK(!memcmp(d->bios_data, rom, sizeof rom));
    CHECK(r1700 == 0x0040);                             /* PRAMIN window restored */
    CHECK(r88050 & 1);                                  /* ROM shadow restored */
    CHECK(prom_reads > 0 && prom_reads_shadowed == 0);  /* PROM only read with the shadow off */
    CHECK(d->bios.noutputs == 5 && d->bios.output[1].i2c_index == 1);

    const struct nv_dp_probe *p = &d->dp[1];           /* DCB 1: DP on AUX channel 1 */
    CHECK(p->probed && p->aux == 1 && p->sink);
    CHECK(p->dpcd_rc == NV_OK && p->dpcd[0] == 0x14 && (p->dpcd[2] & 0x1f) == 4);
    CHECK(p->link_cfg[0] == 0x14 && p->status[0] == 0x41);
    CHECK(p->edid_rc == NV_OK && p->edid_len == 256 && !memcmp(p->edid, edid, 256));
    CHECK(aux_pad[1][0] == 0x00000002 && aux_pad[1][1] == 0);              /* pad switched to AUX */
    CHECK(aux_dpcd_auto[1] == 0x00010000);                                  /* auto-DPCD back on */
    CHECK(d->dp[0].probed && !d->dp[0].sink && d->dp[0].edid_rc == NV_ERR_NOSINK);
    CHECK(!d->dp[4].probed);                                                /* TMDS output: no AUX */
    CHECK(d->dp[1].route_sor == 1 && d->dp[1].route_link == 0);

    CHECK(d->head_mask == 0xf && d->sor_mask == 0xf && d->head_count == 4);
    CHECK(d->head[0].active && d->head[0].htotal == 2720 && d->head[0].vtotal == 1481);
    CHECK(d->head[0].pixel_hz == 241500000 && d->head[0].depth == 24 && d->head[0].offset == 0x00100000);
    CHECK(!d->head[1].active);
    CHECK(d->sor[1].heads == 1 && d->sor[1].proto == 8);

    static char rep[64 * 1024];
    size_t n = nv_report(d, rep, sizeof rep);
    CHECK(n > 0 && n < sizeof rep && strstr(rep, "GP106") && strstr(rep, "preferred 2560x1440") &&
          strstr(rep, "head 0: ACTIVE, raster 2720x1481") && strstr(rep, "routed to SOR 1"));
    CHECK(nv_report(d, rep, 100) == 100);               /* truncation stays in bounds */
    n = nv_report(d, rep, sizeof rep);
    fwrite(rep, 1, n, stdout);
    free(d->bios_data);
    free(d);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
