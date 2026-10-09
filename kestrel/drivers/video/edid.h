/* =============================================================================
 *  edid.h -- standalone EDID 1.3/1.4 base block parser
 *
 *  Freestanding: needs only <stdint.h>, <stdbool.h>. Extracts what a mode
 *  setter needs from the 128-byte base block: the manufacturer, product
 *  code, the monitor name descriptor and the preferred (native) timing,
 *  which by definition is the first detailed timing descriptor.
 * ============================================================================= */
#ifndef EDID_PARSER_H
#define EDID_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#define EDID_BLOCK_SIZE     128

struct edid_info {
    char     vendor[4];         /* 3-letter PNP ID, e.g. "RHT", "DEL"           */
    uint16_t product;
    uint32_t serial;
    uint8_t  version, revision; /* 1.3, 1.4                                      */
    char     name[14];          /* monitor name descriptor (0xFC), or ""         */
    int      native_width;      /* preferred timing: active pixels               */
    int      native_height;
    uint32_t pixel_clock_khz;
    int      refresh_hz;        /* from the pixel clock and blanking totals      */
    int      width_mm, height_mm;
};

/* Parse a base block. Returns false (and leaves *out zeroed) when the header
 * or checksum is wrong or no preferred timing is present. */
bool edid_parse(const uint8_t block[EDID_BLOCK_SIZE], struct edid_info *out);

#endif /* EDID_PARSER_H */
