/* =============================================================================
 *  nvidia.h -- NVIDIA GPU probe for Kestrel, stage 1 (detection only)
 *
 *  Freestanding: needs only <stdint.h>, <stddef.h>, <stdbool.h> and the
 *  platform hooks below. Everything here reads the hardware; the only state
 *  it changes is restored or harmless:
 *    - the PRAMIN window (0x001700) while the VBIOS is copied, then restored
 *    - the PCI ROM shadow bit (0x088050) while PROM is read, then restored
 *    - DP AUX pads switched to AUX mode and AUX transactions (DPCD and EDID
 *      reads), exactly what nouveau does when it probes connectors
 *
 *  What it finds: chip and VRAM size; the VBIOS (with its DCB output,
 *  connector and I2C/AUX tables, see nvbios.h); for every DisplayPort output
 *  whether a monitor is attached, its DPCD capabilities, the link the
 *  firmware trained and its EDID; and the display engine state the firmware
 *  (UEFI GOP / VBIOS) left behind: which head drives which SOR, with what
 *  raster, pixel clock and scan-out surface.
 *
 *  Register knowledge comes from nouveau (drivers/gpu/drm/nouveau, MIT
 *  licence, Copyright Red Hat Inc.): device/base.c (PMC_BOOT_0), fb/gp102.c
 *  (VRAM size), bios/shadow*.c (VBIOS sources), i2c/auxgm200.c and
 *  padgm200.c (DP AUX), disp/gf119.c and gm200.c (head/SOR state, SOR
 *  routing), nvhw/class/cl907d.h (core channel method offsets).
 * ============================================================================= */
#ifndef NVIDIA_PROBE_H
#define NVIDIA_PROBE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "nvbios.h"

#define NV_MAX_HEADS        4
#define NV_MAX_SORS         8
#define NV_BIOS_MAX         (1024 * 1024)
#define NV_EDID_MAX         256

struct nv_platform {
    /* Map `size` bytes of MMIO uncached (BAR0). Required. */
    void    *(*map_mmio)(uint64_t phys, size_t size);
    void     (*delay_us)(uint32_t us);                      /* required */
    void     (*log_putc)(char c);                           /* required */
    /* Zeroed memory for VBIOS copies (NV_BIOS_MAX bytes each). Required. */
    void    *(*alloc)(size_t size);
    void     (*free)(void *p, size_t size);
    /* Optional: copy the PCI expansion ROM (config offset 0x30) into buf,
     * return the number of bytes copied (0 = not available). */
    uint32_t (*read_pci_rom)(void *buf, uint32_t max);
};

struct nv_head_state {                  /* armed core-channel state, 0x640000 + head * 0x300 */
    bool     active;                    /* raster programmed and a SOR attached */
    uint32_t output_resource, control;
    uint16_t htotal, vtotal, hsync_end, vsync_end, hblank_end, vblank_end, hblank_start, vblank_start;
    uint32_t pixel_hz;
    uint32_t offset;                    /* SET_OFFSET: surface address >> 8 */
    uint32_t size, storage, params;
    uint32_t viewport_in, viewport_out;
    int      depth;                     /* bits per pixel on the wire */
};

struct nv_sor_state {
    uint32_t ctrl;                      /* SOR_SET_CONTROL (armed) */
    uint8_t  heads;                     /* owner heads (bits 3:0) */
    uint8_t  proto;                     /* 0 LVDS, 1/2/5 TMDS, 8/9 DP (link A/B) */
};

struct nv_dp_probe {
    bool     probed;
    int      aux;                       /* AUX channel, -1 if none */
    bool     sink;                      /* AUX reports a sink (HPD) */
    int      dpcd_rc;
    uint8_t  dpcd[16];                  /* DPCD 0x000-0x00f: receiver capabilities */
    uint8_t  link_cfg[2];               /* DPCD 0x100-0x101: rate and lanes set by the firmware */
    uint8_t  status[6];                 /* DPCD 0x200-0x205: sink count, lane status */
    int      edid_rc;
    uint32_t edid_len;
    uint8_t  edid[NV_EDID_MAX];
    int      route_sor;                 /* SOR this output is routed to, -1 if none */
    int      route_link;
};

struct nv_device {
    /* PCI */
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id, subsys_vendor, subsys_id;
    uint64_t bar0, bar1, bar1_size;
    volatile uint8_t *mmio;

    /* chip */
    uint32_t boot0;
    uint16_t chipset;                   /* e.g. 0x136 = GP106 */
    uint8_t  chiprev;
    const char *chip_name, *family;
    uint64_t vram_bytes;
    bool     display_present;
    bool     aux_supported;             /* GM20x / GP10x AUX block */
    bool     state_supported;           /* GF119..GP10x core-channel state layout */

    /* VBIOS */
    uint8_t *bios_data;
    uint32_t bios_size;
    const char *bios_source;
    int      bios_score;
    bool     bios_ok;
    struct nvbios bios;

    /* display engine as left by the firmware */
    uint32_t head_mask, sor_mask;
    int      head_count;
    struct nv_head_state head[NV_MAX_HEADS];
    struct nv_sor_state  sor[NV_MAX_SORS];

    /* per DCB output */
    struct nv_dp_probe dp[NVBIOS_MAX_OUTPUTS];
};

/* Probe one GPU. The caller fills the PCI fields (ids, bar0, bar1) first;
 * BAR0 must have memory decoding enabled. Returns 0 or a negative error;
 * even on error the device fields say how far it got. */
int    nv_probe(struct nv_device *d, const struct nv_platform *plat);

/* Human-readable report of everything nv_probe() found. */
size_t nv_report(const struct nv_device *d, char *buf, size_t cap);

/* Read `len` bytes of DPCD (native AUX) or EDID-style I2C (over AUX) on
 * AUX channel `ch`. Return 0 or a negative error. */
int    nv_aux_dpcd_read(struct nv_device *d, int ch, uint32_t addr, uint8_t *buf, uint32_t len);
int    nv_aux_i2c_read(struct nv_device *d, int ch, uint8_t i2c_addr, uint8_t offset, uint8_t *buf, uint32_t len);

#define NV_OK            0
#define NV_ERR_NODEV    -1
#define NV_ERR_TIMEOUT  -2
#define NV_ERR_IO       -3
#define NV_ERR_NACK     -4
#define NV_ERR_NOSINK   -5
#define NV_ERR_NOMEM    -6
#define NV_ERR_NOBIOS   -7

const char *nv_strerror(int err);

#endif /* NVIDIA_PROBE_H */
