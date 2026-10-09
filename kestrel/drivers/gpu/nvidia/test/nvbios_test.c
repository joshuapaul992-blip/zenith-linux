/* Host unit test for nvbios.c.
 *
 * Builds a synthetic ROM laid out like a Pascal board's:
 *   0x00000  x86 image (type 0x00, 32 KiB, checksummed), BIT table with 'i'
 *   0x08000  EFI image (type 0x03, 32 KiB)
 *   0x10000  extended image (type 0xe0, 32 KiB, last): DCB 4.1, connector
 *            and I2C tables. The pointer at 0x36 (0x9000) lies past image 0
 *            and must be remapped into the extended image, as nouveau does.
 * Optionally parses a real ROM given on the command line and prints it.
 *
 *   cc -Idrivers/gpu/nvidia drivers/gpu/nvidia/test/nvbios_test.c drivers/gpu/nvidia/nvbios.c
 */
#include "nvbios.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#include "synthrom.h"

static void dump(const struct nvbios *b)
{
    printf("images:");
    for (int i = 0; i < b->nimages; i++)
        printf(" [%05x %6u type %02x%s%s]", b->image[i].base, b->image[i].size, b->image[i].type,
               b->image[i].type == 0 ? (b->image[i].checksum_ok ? " sum ok" : " BAD SUM") : "", b->image[i].last ? " last" : "");
    printf("\nversion %02x.%02x.%02x.%02x.%02x, BIT at %x, DCB %02x at %x (%d entries)\n",
           b->version[0], b->version[1], b->version[2], b->version[3], b->version[4], b->bit_offset,
           b->dcb_ver, b->dcb, b->dcb_cnt);
    for (int i = 0; i < b->noutputs; i++) {
        const struct nvbios_output *o = &b->output[i];
        printf("  out %d: %-6s or %x link %x heads %x conn %d i2c %d loc %d", o->index, nvbios_output_type_name(o->type),
               o->or, o->link, o->heads, o->connector, o->i2c_index, o->location);
        if (o->type == DCB_OUTPUT_DP) printf("  dp %d lanes x %d.%02d Gbps", o->dp_link_nr, o->dp_link_bw * 27 / 100, o->dp_link_bw * 27 % 100);
        printf("\n");
    }
    for (int i = 0; i < b->nconns; i++)
        printf("  conn %d: %s (0x%02x) hpd %d\n", i, nvbios_conn_type_name(b->conn[i].type), b->conn[i].type, b->conn[i].hpd);
    for (int i = 0; i < b->ni2c; i++)
        printf("  i2c %d: type %02x drive %02x aux %02x\n", i, b->i2c[i].type, b->i2c[i].drive, b->i2c[i].auxch);
}

int main(int argc, char **argv)
{
    struct nvbios b;
    build();
    int score;
    struct nvbios_image im[8];
    CHECK(nvbios_images(rom, sizeof rom, im, 8, &score) == 3);
    CHECK(im[0].checksum_ok && im[2].type == 0xe0 && im[2].last);
    CHECK(nvbios_parse(&b, rom, sizeof rom));
    CHECK(b.image0_size == 0x8000 && b.imaged_addr == 0x10000);
    CHECK(b.bit_offset == 0x1000);
    CHECK(b.has_version && b.version[0] == 0x86 && b.version[1] == 0x06 && b.version[4] == 0x0b);
    CHECK(b.dcb == 0x9000 && b.dcb_ver == 0x41 && b.dcb_cnt == 6);
    CHECK(b.noutputs == 5);
    CHECK(b.output[2].type == DCB_OUTPUT_DP && b.output[2].or == 4 && b.output[2].connector == 2 && b.output[2].i2c_index == 2);
    CHECK(b.output[2].dp_link_nr == 4 && b.output[2].dp_link_bw == 0x1e && b.output[2].link == 1 && b.output[2].heads == 0xf);
    CHECK(b.output[4].type == DCB_OUTPUT_TMDS && b.output[4].link == 2);
    CHECK(b.nconns == 4 && b.conn[3].type == DCB_CONNECTOR_DP && b.conn[3].hpd == 1);
    CHECK(b.ni2c == 5 && b.i2c[1].type == DCB_I2C_PMGR && b.i2c[1].auxch == 1 && b.i2c[1].drive == 1 && b.i2c[1].share == 1);
    CHECK(b.i2c[4].type == DCB_I2C_UNUSED);
    rom[0x123] ^= 1;                            /* checksum must now fail */
    CHECK(nvbios_images(rom, sizeof rom, im, 8, NULL) == 3 && !im[0].checksum_ok);
    CHECK(!nvbios_parse(&b, rom + 1, sizeof rom - 1));     /* no 55 AA at offset 0 */

    if (argc > 1) {                             /* a real ROM dump */
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        static uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (!nvbios_parse(&b, buf, (uint32_t)n)) { printf("%s: not a usable VBIOS\n", argv[1]); return 1; }
        dump(&b);
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
