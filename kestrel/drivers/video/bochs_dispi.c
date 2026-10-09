/* =============================================================================
 *  bochs_dispi.c -- standalone DISPI mode setter (QEMU bochs-display,
 *                   secondary-vga)
 *
 *  Reference: QEMU hw/display/vga_int.h and bochs-display.c (register
 *  layout of the MMIO BAR), Bochs vbe.h (DISPI interface).
 * ============================================================================= */
#include "bochs_dispi.h"

static inline uint16_t rd(volatile void *mmio, unsigned index)
{
    return *(volatile uint16_t *)((volatile uint8_t *)mmio + DISPI_MMIO_REGS + index * 2);
}
static inline void wr(volatile void *mmio, unsigned index, uint16_t v)
{
    *(volatile uint16_t *)((volatile uint8_t *)mmio + DISPI_MMIO_REGS + index * 2) = v;
}

bool dispi_present(volatile void *mmio)
{
    uint16_t id = rd(mmio, DISPI_INDEX_ID);
    return id >= DISPI_ID0 && id <= DISPI_ID5;
}

void dispi_read_edid(volatile void *mmio, uint8_t out[128])
{
    volatile uint8_t *e = (volatile uint8_t *)mmio + DISPI_MMIO_EDID;
    for (int i = 0; i < 128; i++) out[i] = e[i];
}

bool dispi_set_mode(volatile void *mmio, int width, int height, struct dispi_mode *out)
{
    if (width <= 0 || height <= 0 || width > DISPI_MAX_XRES || height > DISPI_MAX_YRES) return false;
    uint32_t vram = (uint32_t)rd(mmio, DISPI_INDEX_VIDEO_MEMORY_64K) << 16;
    if (vram && (uint64_t)width * (uint64_t)height * 4 > vram) return false;

    /* The adapter latches XRES/YRES/BPP only while disabled. */
    wr(mmio, DISPI_INDEX_ENABLE, 0);
    wr(mmio, DISPI_INDEX_BPP, 32);
    wr(mmio, DISPI_INDEX_XRES, (uint16_t)width);
    wr(mmio, DISPI_INDEX_YRES, (uint16_t)height);
    wr(mmio, DISPI_INDEX_BANK, 0);
    wr(mmio, DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);
    wr(mmio, DISPI_INDEX_X_OFFSET, 0);
    wr(mmio, DISPI_INDEX_Y_OFFSET, 0);
    wr(mmio, DISPI_INDEX_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    /* An adapter with a VGA core (secondary-vga) shows a blank screen until
     * the attribute controller's palette address source bit is set: read
     * Input Status 1 (0x3DA) to reset the index/data flip-flop, then write
     * 0x20 to 0x3C0. bochs-display has no VGA core and ignores both. */
    volatile uint8_t *vga = (volatile uint8_t *)mmio + DISPI_MMIO_VGA;
    (void)vga[0x3DA - 0x3C0];
    vga[0x3C0 - 0x3C0] = 0x20;

    /* Read back: the adapter clamps values it cannot honour. */
    if (rd(mmio, DISPI_INDEX_XRES) != width || rd(mmio, DISPI_INDEX_YRES) != height ||
        rd(mmio, DISPI_INDEX_BPP) != 32 || !(rd(mmio, DISPI_INDEX_ENABLE) & DISPI_ENABLED))
        return false;
    out->width      = width;
    out->height     = height;
    out->pitch      = rd(mmio, DISPI_INDEX_VIRT_WIDTH) * 4;
    out->vram_bytes = vram;
    return true;
}
