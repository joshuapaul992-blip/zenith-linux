/* =============================================================================
 *  nvbios.c -- NVIDIA video BIOS parser (see nvbios.h)
 *
 *  Derived from nouveau's nvkm/subdev/bios (image.c, pcir.c, npde.c, bit.c,
 *  base.c, dcb.c, conn.c, i2c.c). Copyright 2012 Red Hat Inc., MIT licence.
 * ============================================================================= */
#include "nvbios.h"

static void zero(void *p, uint32_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }

static inline uint8_t  raw8 (const uint8_t *d, uint32_t size, uint32_t a) { return a < size ? d[a] : 0; }
static inline uint16_t raw16(const uint8_t *d, uint32_t size, uint32_t a) { return (uint16_t)(raw8(d, size, a) | raw8(d, size, a + 1) << 8); }
static inline uint32_t raw32(const uint8_t *d, uint32_t size, uint32_t a) { return raw16(d, size, a) | (uint32_t)raw16(d, size, a + 2) << 16; }

/* nouveau nvbios_addr(): table pointers beyond image 0 point into the
 * extended image. */
static uint32_t map(const struct nvbios *b, uint32_t a)
{
    if (a >= b->image0_size && b->imaged_addr) a = a - b->image0_size + b->imaged_addr;
    return a;
}
uint8_t  nvbios_rd08(const struct nvbios *b, uint32_t a) { return raw8(b->data, b->size, map(b, a)); }
uint16_t nvbios_rd16(const struct nvbios *b, uint32_t a) { return raw16(b->data, b->size, map(b, a)); }
uint32_t nvbios_rd32(const struct nvbios *b, uint32_t a) { return raw32(b->data, b->size, map(b, a)); }

/* ---- images (PCI expansion ROM chain, PCIR + NVIDIA NPDE extension) ------ */
uint8_t nvbios_checksum(const uint8_t *d, uint32_t len)
{
    uint8_t sum = 0;
    while (len--) sum = (uint8_t)(sum + *d++);
    return sum;
}

bool nvbios_image_header(const uint8_t *d, uint32_t size, uint32_t base, struct nvbios_image *im)
{
    zero(im, sizeof *im);
    im->base = base;
    uint16_t sig = raw16(d, size, base);
    if (sig != 0xaa55 && sig != 0xbb77 && sig != 0x4e56) return false;     /* 55 AA, 77 BB, "VN" */
    uint32_t pcir = raw16(d, size, base + 0x18);
    if (!pcir) return false;
    pcir += base;
    uint32_t psig = raw32(d, size, pcir);
    if (psig != 0x52494350 && psig != 0x53494752 && psig != 0x5344504e) return false;  /* PCIR RGIS NPDS */
    uint16_t hdr = raw16(d, size, pcir + 0x0a);
    im->vendor = raw16(d, size, pcir + 0x04);
    im->device = raw16(d, size, pcir + 0x06);
    im->size   = (uint32_t)raw16(d, size, pcir + 0x10) * 512;
    im->type   = raw8(d, size, pcir + 0x14);
    im->last   = raw8(d, size, pcir + 0x15) & 0x80;
    if (im->type != 0x70) {
        uint32_t npde = (pcir + hdr + 0x0f) & ~0x0fu;
        if (raw32(d, size, npde) == 0x4544504e) {                          /* NPDE */
            im->size = (uint32_t)raw16(d, size, npde + 0x08) * 512;
            im->last = raw8(d, size, npde + 0x0a) & 0x80;
        }
    } else {
        im->last = true;
    }
    return im->size != 0;
}

static bool image_at(const uint8_t *d, uint32_t size, uint32_t base, struct nvbios_image *im)
{
    if (!nvbios_image_header(d, size, base, im) || base + im->size > size) return false;
    if (im->type == 0x00) im->checksum_ok = nvbios_checksum(d + base, im->size) == 0;
    return true;
}

int nvbios_images(const uint8_t *d, uint32_t size, struct nvbios_image *out, int max, int *score)
{
    int n = 0, sc = 0;
    uint32_t base = 0;
    struct nvbios_image im;
    while (n < max && image_at(d, size, base, &im)) {
        out[n++] = im;
        sc += 1 + (im.type != 0x00 || im.checksum_ok ? 3 : 1);
        if (im.last) break;
        base += im.size;
    }
    if (score) *score = sc;
    return n;
}

/* ---- BIT -------------------------------------------------------------------- */
static uint32_t findstr(const uint8_t *d, uint32_t size, const char *s, uint32_t len)
{
    for (uint32_t i = 0; i + len <= size; i++) {
        uint32_t j = 0;
        while (j < len && d[i + j] == (uint8_t)s[j]) j++;
        if (j == len) return i;
    }
    return 0;
}

bool nvbios_bit_entry(const struct nvbios *b, char id, uint8_t *version, uint16_t *offset, uint16_t *length)
{
    if (!b->bit_offset) return false;
    uint8_t entries = nvbios_rd08(b, b->bit_offset + 10);
    uint8_t esize = nvbios_rd08(b, b->bit_offset + 9);
    uint32_t e = b->bit_offset + 12;
    while (entries--) {
        if (nvbios_rd08(b, e) == (uint8_t)id) {
            if (version) *version = nvbios_rd08(b, e + 1);
            if (length)  *length  = nvbios_rd16(b, e + 2);
            if (offset)  *offset  = nvbios_rd16(b, e + 4);
            return true;
        }
        e += esize;
    }
    return false;
}

/* ---- DCB (version 3.0 - 4.1) ------------------------------------------------- */
static bool dcb_table(struct nvbios *b)
{
    uint32_t dcb = nvbios_rd16(b, 0x36);
    if (!dcb) return false;
    uint8_t ver = nvbios_rd08(b, dcb);
    if (ver < 0x30 || ver >= 0x42) return false;                       /* pre-NV50 and unknown layouts */
    if (nvbios_rd32(b, dcb + 6) != 0x4edcbdcb) return false;
    b->dcb = dcb;
    b->dcb_ver = ver;
    b->dcb_hdr = nvbios_rd08(b, dcb + 1);
    b->dcb_cnt = nvbios_rd08(b, dcb + 2);
    b->dcb_len = nvbios_rd08(b, dcb + 3);
    return b->dcb_len >= 8;
}

static void parse_output(struct nvbios *b, int idx, uint32_t e)
{
    struct nvbios_output *o = &b->output[b->noutputs++];
    uint32_t conn = nvbios_rd32(b, e), conf = nvbios_rd32(b, e + 4);
    zero(o, sizeof *o);
    o->index     = idx;
    o->raw_conn  = conn;
    o->raw_conf  = conf;
    o->or        = (uint8_t)((conn & 0x0f000000) >> 24);
    o->location  = (uint8_t)((conn & 0x00300000) >> 20);
    o->bus       = (uint8_t)((conn & 0x000f0000) >> 16);
    o->connector = (uint8_t)((conn & 0x0000f000) >> 12);
    o->heads     = (uint8_t)((conn & 0x00000f00) >> 8);
    o->i2c_index = (uint8_t)((conn & 0x000000f0) >> 4);
    o->type      = (uint8_t)(conn & 0x0000000f);
    if (b->dcb_ver < 0x40) return;
    switch (o->type) {
    case DCB_OUTPUT_DP:
        switch (conf & 0x00e00000) {
        case 0x00000000: o->dp_link_bw = 0x06; break;                   /* 1.62 Gbps */
        case 0x00200000: o->dp_link_bw = 0x0a; break;                   /* 2.7 */
        case 0x00400000: o->dp_link_bw = 0x14; break;                   /* 5.4 */
        default:         o->dp_link_bw = 0x1e; break;                   /* 8.1 */
        }
        switch ((conf & 0x0f000000) >> 24) {
        case 0xf: case 0x4: o->dp_link_nr = 4; break;
        case 0x3: case 0x2: o->dp_link_nr = 2; break;
        default:            o->dp_link_nr = 1; break;
        }
        /* fall through */
    case DCB_OUTPUT_TMDS:
    case DCB_OUTPUT_LVDS:
        o->link = (uint8_t)((conf & 0x00000030) >> 4);
        break;
    default:
        break;
    }
}

static void parse_outputs(struct nvbios *b)
{
    for (int i = 0; i < b->dcb_cnt && b->noutputs < NVBIOS_MAX_OUTPUTS; i++) {
        uint32_t e = b->dcb + b->dcb_hdr + (uint32_t)i * b->dcb_len;
        uint32_t w = nvbios_rd32(b, e);
        if (w == 0x00000000 || w == 0xffffffff) break;
        uint8_t type = (uint8_t)(w & 0x0f);
        if (type == DCB_OUTPUT_UNUSED) continue;
        if (type == DCB_OUTPUT_EOL) break;
        parse_output(b, i, e);
    }
}

static void parse_conns(struct nvbios *b)
{
    if (b->dcb_hdr < 0x16) return;
    uint32_t t = nvbios_rd16(b, b->dcb + 0x14);
    if (!t) return;
    uint8_t ver = nvbios_rd08(b, t), hdr = nvbios_rd08(b, t + 1), cnt = nvbios_rd08(b, t + 2), len = nvbios_rd08(b, t + 3);
    if (ver != 0x30 && ver != 0x40) return;
    b->conn_table = t;
    b->conn_ver = ver;
    for (int i = 0; i < cnt && b->nconns < NVBIOS_MAX_CONNS; i++) {
        uint32_t e = t + hdr + (uint32_t)i * len;
        struct nvbios_conn *c = &b->conn[b->nconns++];
        c->type     = nvbios_rd08(b, e);
        c->location = nvbios_rd08(b, e + 1) & 0x0f;
        c->hpd      = (nvbios_rd08(b, e + 1) & 0x30) >> 4;
        c->dp       = (nvbios_rd08(b, e + 1) & 0xc0) >> 6;
        if (len >= 4) {
            c->hpd |= (uint8_t)((nvbios_rd08(b, e + 2) & 0x03) << 2);
            c->dp  |= nvbios_rd08(b, e + 2) & 0x0c;
            c->di   = (nvbios_rd08(b, e + 2) & 0xf0) >> 4;
            c->hpd |= (uint8_t)((nvbios_rd08(b, e + 3) & 0x07) << 4);
        }
    }
}

static void parse_i2c(struct nvbios *b)
{
    uint32_t t = nvbios_rd16(b, b->dcb + 4);
    if (!t) return;
    uint8_t ver = nvbios_rd08(b, t), hdr = nvbios_rd08(b, t + 1), cnt = nvbios_rd08(b, t + 2), len = nvbios_rd08(b, t + 3);
    b->i2c_table = t;
    b->i2c_ver = ver;
    for (int i = 0; i < cnt && b->ni2c < NVBIOS_MAX_I2C; i++) {
        uint32_t e = t + hdr + (uint32_t)i * len;
        struct nvbios_i2c *c = &b->i2c[b->ni2c++];
        c->drive = c->auxch = c->share = DCB_I2C_UNUSED;
        if (ver >= 0x41) {
            uint32_t v = nvbios_rd32(b, e);
            uint8_t port = v & 0x1f, aux = (v >> 5) & 0x1f;
            c->type = (port == 0x1f && aux == 0x1f) ? DCB_I2C_UNUSED : DCB_I2C_PMGR;
        } else {
            c->type = nvbios_rd08(b, e + 3);
        }
        switch (c->type) {
        case DCB_I2C_NV04_BIT: c->drive = nvbios_rd08(b, e); break;
        case DCB_I2C_NV4E_BIT: c->drive = nvbios_rd08(b, e + 1); break;
        case DCB_I2C_NVIO_BIT:
            c->drive = nvbios_rd08(b, e) & 0x0f;
            if (nvbios_rd08(b, e + 1) & 0x01) c->share = nvbios_rd08(b, e + 1) >> 1;
            break;
        case DCB_I2C_NVIO_AUX:
            c->auxch = nvbios_rd08(b, e) & 0x0f;
            if (nvbios_rd08(b, e + 1) & 0x01) c->share = c->auxch;
            break;
        case DCB_I2C_PMGR:
            c->drive = (uint8_t)(nvbios_rd16(b, e) & 0x01f);
            if (c->drive == 0x1f) c->drive = DCB_I2C_UNUSED;
            c->auxch = (uint8_t)((nvbios_rd16(b, e) & 0x3e0) >> 5);
            if (c->auxch == 0x1f) c->auxch = DCB_I2C_UNUSED;
            c->share = c->auxch;
            break;
        case DCB_I2C_UNUSED:
            break;
        default:
            c->type = DCB_I2C_UNUSED;
            break;
        }
    }
}

bool nvbios_parse(struct nvbios *b, const uint8_t *data, uint32_t size)
{
    zero(b, sizeof *b);
    b->data = data;
    b->size = size;
    b->nimages = nvbios_images(data, size, b->image, NVBIOS_MAX_IMAGES, NULL);
    if (!b->nimages) return false;
    b->image0_size = b->image[0].size;
    for (int i = 1; i < b->nimages; i++)
        if (b->image[i].type == 0xe0) { b->imaged_addr = b->image[i].base; break; }

    b->bit_offset = findstr(data, size, "\xff\xb8" "BIT", 5);
    uint16_t off, len;
    if (nvbios_bit_entry(b, 'i', NULL, &off, &len) && len >= 4) {
        b->version[0] = nvbios_rd08(b, off + 3);
        b->version[1] = nvbios_rd08(b, off + 2);
        b->version[2] = nvbios_rd08(b, off + 1);
        b->version[3] = nvbios_rd08(b, off + 0);
        b->version[4] = nvbios_rd08(b, off + 4);
        b->has_version = true;
    }
    if (dcb_table(b)) {
        parse_outputs(b);
        parse_conns(b);
        parse_i2c(b);
    }
    return true;
}

const char *nvbios_output_type_name(uint8_t t)
{
    switch (t) {
    case DCB_OUTPUT_ANALOG: return "analog";
    case DCB_OUTPUT_TV:     return "TV";
    case DCB_OUTPUT_TMDS:   return "TMDS";
    case DCB_OUTPUT_LVDS:   return "LVDS";
    case DCB_OUTPUT_DP:     return "DP";
    case DCB_OUTPUT_WFD:    return "WFD";
    default:                return "?";
    }
}

const char *nvbios_conn_type_name(uint8_t t)
{
    switch (t) {
    case DCB_CONNECTOR_VGA:    return "VGA";
    case DCB_CONNECTOR_DVI_I:  return "DVI-I";
    case DCB_CONNECTOR_DVI_D:  return "DVI-D";
    case DCB_CONNECTOR_LVDS:   return "LVDS";
    case DCB_CONNECTOR_DP:     return "DisplayPort";
    case DCB_CONNECTOR_eDP:    return "eDP";
    case DCB_CONNECTOR_mDP:    return "mini-DP";
    case DCB_CONNECTOR_HDMI_0:
    case DCB_CONNECTOR_HDMI_1: return "HDMI";
    case DCB_CONNECTOR_HDMI_C: return "HDMI-C";
    case DCB_CONNECTOR_USB_C:  return "USB-C";
    case DCB_CONNECTOR_NONE:   return "none";
    default:                   return "other";
    }
}
