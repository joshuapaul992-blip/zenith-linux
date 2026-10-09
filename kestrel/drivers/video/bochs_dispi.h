/* =============================================================================
 *  bochs_dispi.h -- standalone mode setter for Bochs/QEMU DISPI display adapters
 *
 *  Freestanding: needs only <stdint.h>, <stdbool.h>. Covers PCI 1234:1111
 *  as QEMU exposes it in `-device bochs-display` and `-device secondary-vga`:
 *
 *      BAR0   linear frame buffer (VRAM)
 *      BAR2   4 KiB MMIO: EDID blob at 0x000, VGA ports 0x3C0-0x3DF at
 *             0x400 (secondary-vga only), DISPI registers at 0x500
 *             (16-bit, index * 2), QEMU extended registers at 0x600
 *
 *  Each adapter drives one monitor, so every adapter is one display head.
 * ============================================================================= */
#ifndef BOCHS_DISPI_H
#define BOCHS_DISPI_H

#include <stdint.h>
#include <stdbool.h>

#define DISPI_PCI_VENDOR        0x1234
#define DISPI_PCI_DEVICE        0x1111

#define DISPI_MMIO_EDID         0x000
#define DISPI_MMIO_VGA          0x400   /* + (port - 0x3C0) */
#define DISPI_MMIO_REGS         0x500
#define DISPI_MMIO_QEMU_EXT     0x600

/* DISPI register indices */
#define DISPI_INDEX_ID          0x0
#define DISPI_INDEX_XRES        0x1
#define DISPI_INDEX_YRES        0x2
#define DISPI_INDEX_BPP         0x3
#define DISPI_INDEX_ENABLE      0x4
#define DISPI_INDEX_BANK        0x5
#define DISPI_INDEX_VIRT_WIDTH  0x6
#define DISPI_INDEX_VIRT_HEIGHT 0x7
#define DISPI_INDEX_X_OFFSET    0x8
#define DISPI_INDEX_Y_OFFSET    0x9
#define DISPI_INDEX_VIDEO_MEMORY_64K 0xA

#define DISPI_ID0               0xB0C0  /* ID values 0xB0C0..0xB0C5 */
#define DISPI_ID5               0xB0C5
#define DISPI_ENABLED           0x01
#define DISPI_LFB_ENABLED       0x40
#define DISPI_NOCLEARMEM        0x80
#define DISPI_MAX_XRES          16000   /* VBE_DISPI_MAX_XRES in QEMU */
#define DISPI_MAX_YRES          12000

struct dispi_mode {
    int      width, height;
    int      pitch;             /* bytes per scan line */
    uint32_t vram_bytes;
};

/* `mmio` is the mapped BAR2. True if the DISPI ID register answers. */
bool dispi_present(volatile void *mmio);

/* Copy the EDID base block (128 bytes) out of the MMIO window. */
void dispi_read_edid(volatile void *mmio, uint8_t out[128]);

/* Program a 32-bpp linear mode. Fails if the adapter rejects the size or its
 * VRAM cannot hold it; reads the mode back to report the real pitch. */
bool dispi_set_mode(volatile void *mmio, int width, int height, struct dispi_mode *out);

#endif /* BOCHS_DISPI_H */
