/* Synthetic Pascal-style VBIOS for the host tests (see nvbios_test.c for
 * the layout). Included by both test programs. */
#ifndef SYNTHROM_H
#define SYNTHROM_H
#include "nvbios.h"
#include <string.h>

static uint8_t rom[0x18000];
static void w8(uint32_t a, uint8_t v)   { rom[a] = v; }
static void w16(uint32_t a, uint16_t v) { rom[a] = (uint8_t)v; rom[a + 1] = (uint8_t)(v >> 8); }
static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)v); w16(a + 2, (uint16_t)(v >> 16)); }

static void image(uint32_t base, uint32_t size, uint8_t type, int last)
{
    w16(base, 0xaa55);
    w16(base + 0x18, 0x40);                     /* PCIR at base + 0x40 */
    uint32_t p = base + 0x40;
    memcpy(&rom[p], "PCIR", 4);
    w16(p + 4, 0x10de); w16(p + 6, 0x1c30);     /* Quadro P2000 */
    w16(p + 0x0a, 0x18);                        /* PCIR length */
    w16(p + 0x10, (uint16_t)(size / 512));
    w8(p + 0x14, type);
    w8(p + 0x15, 0x00);                         /* "last" comes from the NPDE */
    uint32_t n = (p + 0x18 + 0x0f) & ~0x0fu;
    memcpy(&rom[n], "NPDE", 4);
    w16(n + 8, (uint16_t)(size / 512));
    w8(n + 0x0a, last ? 0x80 : 0x00);
}

static void build(void)
{
    memset(rom, 0, sizeof rom);
    image(0x00000, 0x8000, 0x00, 0);
    image(0x08000, 0x8000, 0x03, 0);
    image(0x10000, 0x8000, 0xe0, 1);

    /* BIT at 0x1000 with an 'i' entry pointing at version bytes */
    uint32_t bit = 0x1000;
    rom[bit] = 0xff; rom[bit + 1] = 0xb8; memcpy(&rom[bit + 2], "BIT\0", 4);
    w16(bit + 6, 0x0100); w8(bit + 8, 12); w8(bit + 9, 6); w8(bit + 10, 2);
    w8(bit + 12, 'i'); w8(bit + 13, 2); w16(bit + 14, 5); w16(bit + 16, 0x1100);
    w8(bit + 18, 'I'); w8(bit + 19, 1); w16(bit + 20, 0x10); w16(bit + 22, 0x1200);
    w8(0x1100, 0x40); w8(0x1101, 0x00); w8(0x1102, 0x06); w8(0x1103, 0x86); w8(0x1104, 0x0b);  /* 86.06.00.40.0b */

    /* DCB pointer past image 0: 0x9000 -> 0x11000 in the extended image */
    w16(0x36, 0x9000);
    uint32_t dcb = 0x11000;
    w8(dcb, 0x41); w8(dcb + 1, 0x1b); w8(dcb + 2, 6); w8(dcb + 3, 8);
    w16(dcb + 4, 0x9200);                       /* I2C table  -> 0x11200 */
    w32(dcb + 6, 0x4edcbdcb);
    w16(dcb + 0x14, 0x9100);                    /* connector table -> 0x11100 */
    uint32_t e = dcb + 0x1b;
    for (int i = 0; i < 4; i++, e += 8) {       /* four DP outputs, SOR i, 4 lanes at 8.1 Gbps */
        w32(e,     (1u << (24 + i)) | ((uint32_t)i << 12) | (0xfu << 8) | ((uint32_t)i << 4) | DCB_OUTPUT_DP);
        w32(e + 4, 0x04000000 | 0x00600000 | 0x10);
    }
    w32(e, (1u << 24) | (0u << 12) | (0xfu << 8) | (0u << 4) | DCB_OUTPUT_TMDS);   /* DP++ on connector 0 */
    w32(e + 4, 0x20);
    e += 8;
    w32(e, DCB_OUTPUT_EOL);

    uint32_t conn = 0x11100;                    /* v4.0: 4 connectors, 4 bytes each */
    w8(conn, 0x40); w8(conn + 1, 5); w8(conn + 2, 4); w8(conn + 3, 4);
    for (int i = 0; i < 4; i++) { w8(conn + 5 + i * 4, DCB_CONNECTOR_DP); w8(conn + 6 + i * 4, (uint8_t)(0x10 + i)); }

    uint32_t i2c = 0x11200;                     /* v4.1: PMGR entries, port i, aux i */
    w8(i2c, 0x41); w8(i2c + 1, 5); w8(i2c + 2, 5); w8(i2c + 3, 5);
    for (int i = 0; i < 4; i++) w32(i2c + 5 + i * 5, (uint32_t)i | ((uint32_t)i << 5));
    w32(i2c + 5 + 4 * 5, 0x3ff);                /* both 0x1f: unused */

    uint8_t sum = 0;                            /* fix the x86 image checksum */
    for (int i = 0; i < 0x8000 - 1; i++) sum = (uint8_t)(sum + rom[i]);
    rom[0x7fff] = (uint8_t)-sum;
}

#endif
