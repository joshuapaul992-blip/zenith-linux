/* =============================================================================
 *  edid.c -- standalone EDID base block parser
 *
 *  Reference: VESA E-EDID Standard Release A2 (EDID 1.4), sections 3.1
 *  (header), 3.4 (vendor/product), 3.10 (18-byte descriptors) and 3.10.2
 *  (detailed timing descriptor).
 * ============================================================================= */
#include "edid.h"

static void zero(void *p, unsigned n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }

/* Descriptor text: up to 13 bytes, terminated by 0x0A, padded with spaces. */
static void descriptor_text(char out[14], const uint8_t *d)
{
    int n = 0;
    for (int i = 5; i < 18 && d[i] != 0x0A; i++)
        out[n++] = d[i] >= 0x20 && d[i] < 0x7F ? (char)d[i] : '?';
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

bool edid_parse(const uint8_t b[EDID_BLOCK_SIZE], struct edid_info *e)
{
    static const uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    zero(e, sizeof *e);
    for (int i = 0; i < 8; i++) if (b[i] != header[i]) return false;
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK_SIZE; i++) sum = (uint8_t)(sum + b[i]);
    if (sum) return false;

    uint16_t id = (uint16_t)(b[8] << 8 | b[9]);         /* big endian, 5 bits per letter */
    e->vendor[0] = (char)('A' - 1 + ((id >> 10) & 0x1F));
    e->vendor[1] = (char)('A' - 1 + ((id >> 5) & 0x1F));
    e->vendor[2] = (char)('A' - 1 + (id & 0x1F));
    e->product   = (uint16_t)(b[10] | b[11] << 8);
    e->serial    = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    e->version   = b[18];
    e->revision  = b[19];

    /* Four 18-byte descriptors at 54, 72, 90, 108. A non-zero pixel clock
     * marks a detailed timing; the first one is the preferred mode. */
    for (int i = 0; i < 4; i++) {
        const uint8_t *d = b + 54 + 18 * i;
        uint32_t clock = (uint32_t)(d[0] | d[1] << 8);  /* units of 10 kHz */
        if (clock) {
            if (e->native_width) continue;
            int hact   = d[2] | (d[4] & 0xF0) << 4;
            int hblank = d[3] | (d[4] & 0x0F) << 8;
            int vact   = d[5] | (d[7] & 0xF0) << 4;
            int vblank = d[6] | (d[7] & 0x0F) << 8;
            e->native_width    = hact;
            e->native_height   = vact;
            e->pixel_clock_khz = clock * 10;
            e->width_mm        = d[12] | (d[14] & 0xF0) << 4;
            e->height_mm       = d[13] | (d[14] & 0x0F) << 8;
            uint32_t total = (uint32_t)(hact + hblank) * (uint32_t)(vact + vblank);
            if (total) e->refresh_hz = (int)((clock * 10000u + total / 2) / total);
        } else if (d[3] == 0xFC) {
            descriptor_text(e->name, d);
        }
    }
    return e->native_width > 0 && e->native_height > 0;
}
