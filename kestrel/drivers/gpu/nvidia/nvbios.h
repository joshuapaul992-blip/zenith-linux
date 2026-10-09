/* =============================================================================
 *  nvbios.h -- NVIDIA video BIOS parser (images, BIT, DCB 4.x tables)
 *
 *  Freestanding and hardware independent: works on a byte buffer holding a
 *  VBIOS image, so it is unit-tested on the host (test/nvbios_test.c).
 *
 *  Table layouts follow nouveau (drivers/gpu/drm/nouveau/nvkm/subdev/bios:
 *  image.c, pcir.c, npde.c, bit.c, dcb.c, conn.c, i2c.c), Copyright Red Hat
 *  Inc., MIT licence; see third_party/nouveau/LICENSE.
 * ============================================================================= */
#ifndef NVBIOS_H
#define NVBIOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NVBIOS_MAX_IMAGES   8
#define NVBIOS_MAX_OUTPUTS  16
#define NVBIOS_MAX_CONNS    16
#define NVBIOS_MAX_I2C      16

/* DCB output types (DCB 4.x "type" field) */
#define DCB_OUTPUT_ANALOG   0x0
#define DCB_OUTPUT_TV       0x1
#define DCB_OUTPUT_TMDS     0x2
#define DCB_OUTPUT_LVDS     0x3
#define DCB_OUTPUT_DP       0x6
#define DCB_OUTPUT_WFD      0x8
#define DCB_OUTPUT_EOL      0xe
#define DCB_OUTPUT_UNUSED   0xf

/* connector table types */
#define DCB_CONNECTOR_VGA       0x00
#define DCB_CONNECTOR_DVI_I     0x30
#define DCB_CONNECTOR_DVI_D     0x31
#define DCB_CONNECTOR_LVDS      0x40
#define DCB_CONNECTOR_DP        0x46
#define DCB_CONNECTOR_eDP       0x47
#define DCB_CONNECTOR_mDP       0x48
#define DCB_CONNECTOR_HDMI_0    0x60
#define DCB_CONNECTOR_HDMI_1    0x61
#define DCB_CONNECTOR_HDMI_C    0x63
#define DCB_CONNECTOR_USB_C     0x71
#define DCB_CONNECTOR_NONE      0xff

/* I2C/CCB entry types */
#define DCB_I2C_NV04_BIT    0x00
#define DCB_I2C_NV4E_BIT    0x04
#define DCB_I2C_NVIO_BIT    0x05
#define DCB_I2C_NVIO_AUX    0x06
#define DCB_I2C_PMGR        0x80
#define DCB_I2C_UNUSED      0xff

struct nvbios_image {
    uint32_t base, size;
    uint8_t  type;              /* PCIR code type: 0x00 x86, 0x03 EFI, 0x70 NBSI, 0xe0 extended */
    bool     last;
    bool     checksum_ok;       /* type 0x00 only */
    uint16_t vendor, device;
};

struct nvbios_output {          /* one DCB output entry */
    int      index;
    uint8_t  type;              /* DCB_OUTPUT_*            */
    uint8_t  i2c_index;         /* -> I2C/CCB table        */
    uint8_t  heads;             /* bitmask                 */
    uint8_t  connector;         /* -> connector table      */
    uint8_t  bus;
    uint8_t  location;          /* 0 = on chip             */
    uint8_t  or;                /* output resource bitmask (SOR n) */
    uint8_t  link;              /* sub-link mask (TMDS/DP) */
    uint8_t  dp_link_bw;        /* 0x06/0x0a/0x14/0x1e (x 270 MHz) */
    uint8_t  dp_link_nr;        /* lanes */
    uint32_t raw_conn, raw_conf;
};

struct nvbios_conn {
    uint8_t type;               /* DCB_CONNECTOR_*         */
    uint8_t location;
    uint8_t hpd;                /* HPD GPIO function bits  */
    uint8_t dp;
    uint8_t di;
};

struct nvbios_i2c {
    uint8_t type;               /* DCB_I2C_*               */
    uint8_t drive;              /* I2C port, or 0xff       */
    uint8_t auxch;              /* DP AUX channel, or 0xff */
    uint8_t share;              /* hybrid pad id, or 0xff  */
};

struct nvbios {
    const uint8_t *data;
    uint32_t size;

    /* images in the ROM; addresses past image 0 are remapped into the
     * extended (type 0xe0) image, as nouveau does */
    int      nimages;
    struct nvbios_image image[NVBIOS_MAX_IMAGES];
    uint32_t image0_size, imaged_addr;

    uint32_t bit_offset;        /* "\xff\xb8BIT" table, 0 if none */
    uint8_t  version[5];        /* major, chip, minor, micro, patch */
    bool     has_version;

    /* DCB */
    uint32_t dcb;               /* table offset, 0 if none */
    uint8_t  dcb_ver, dcb_hdr, dcb_cnt, dcb_len;
    int      noutputs;
    struct nvbios_output output[NVBIOS_MAX_OUTPUTS];

    uint32_t conn_table;
    uint8_t  conn_ver;
    int      nconns;
    struct nvbios_conn conn[NVBIOS_MAX_CONNS];

    uint32_t i2c_table;
    uint8_t  i2c_ver;
    int      ni2c;
    struct nvbios_i2c i2c[NVBIOS_MAX_I2C];
};

/* Validate the image chain of a ROM. Returns the number of images found
 * (0 = not a VBIOS). `score` (may be NULL) rates it like nouveau's shadow
 * code: higher is better; a type-0 image with a good checksum scores 3. */
int  nvbios_images(const uint8_t *data, uint32_t size, struct nvbios_image *out, int max, int *score);

/* One image header at `base` (signature, PCIR, NPDE): size, type and
 * "last" flag, without requiring the image body to be present yet. */
bool nvbios_image_header(const uint8_t *data, uint32_t size, uint32_t base, struct nvbios_image *out);
uint8_t nvbios_checksum(const uint8_t *data, uint32_t len);

/* Parse everything: images, BIT, version, DCB outputs, connectors, I2C.
 * Returns false if the buffer is not a usable VBIOS. */
bool nvbios_parse(struct nvbios *b, const uint8_t *data, uint32_t size);

uint8_t  nvbios_rd08(const struct nvbios *b, uint32_t addr);
uint16_t nvbios_rd16(const struct nvbios *b, uint32_t addr);
uint32_t nvbios_rd32(const struct nvbios *b, uint32_t addr);

/* BIT table entry by id ('i' = version, 'I' = init, 'p' = PMU ...). */
bool nvbios_bit_entry(const struct nvbios *b, char id, uint8_t *version, uint16_t *offset, uint16_t *length);

const char *nvbios_output_type_name(uint8_t type);
const char *nvbios_conn_type_name(uint8_t type);

#endif /* NVBIOS_H */
