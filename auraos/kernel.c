/*
 * AuraOS - kernel.c
 *
 * Phase 1 freestanding C kernel baseline.
 *
 * Entered from boot.asm (long_mode_start) in 64-bit long mode with:
 *   - interrupts disabled (no IDT is loaded yet),
 *   - physical 0x000000 - 0x1FFFFF identity mapped,
 *   - a 16 KiB stack, 16-byte aligned before the call,
 *   - System V AMD64 arguments: RDI = Multiboot2 magic, RSI = MBI address.
 *
 * Only freestanding headers are used.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Compile-time environment checks                                           */
/* ------------------------------------------------------------------------- */

_Static_assert(sizeof(uint8_t)   == 1, "uint8_t must be 1 byte");
_Static_assert(sizeof(uint16_t)  == 2, "uint16_t must be 2 bytes");
_Static_assert(sizeof(uint32_t)  == 4, "uint32_t must be 4 bytes");
_Static_assert(sizeof(uint64_t)  == 8, "uint64_t must be 8 bytes");
_Static_assert(sizeof(uintptr_t) == 8, "AuraOS targets x86_64 (LP64) only");
_Static_assert(sizeof(size_t)    == 8, "size_t must be 64-bit on x86_64");

/* ------------------------------------------------------------------------- */
/* Boot hand-off contract (must match boot.asm and linker.ld)                */
/* ------------------------------------------------------------------------- */

#define MB2_BOOTLOADER_MAGIC     ((uint32_t)0x36D76289u)
#define BOOT_IDENTITY_MAP_END    ((uint64_t)0x00200000u) /* 2 MiB          */
#define MB2_INFO_ALIGN           ((uint64_t)8u)
#define MB2_INFO_MIN_SIZE        ((uint32_t)16u) /* header (8) + end tag (8) */

/* ------------------------------------------------------------------------- */
/* VGA text mode                                                             */
/* ------------------------------------------------------------------------- */

#define VGA_TEXT_BUFFER_ADDR     ((uintptr_t)0xB8000u)
#define VGA_WIDTH                ((size_t)80u)
#define VGA_HEIGHT               ((size_t)25u)
#define VGA_CELL_COUNT           (VGA_WIDTH * VGA_HEIGHT)
#define VGA_BUFFER_BYTES         (VGA_CELL_COUNT * sizeof(uint16_t))

_Static_assert(VGA_TEXT_BUFFER_ADDR + VGA_BUFFER_BYTES <= BOOT_IDENTITY_MAP_END,
               "VGA text buffer must lie inside the boot identity mapping");
_Static_assert((VGA_TEXT_BUFFER_ADDR % sizeof(uint16_t)) == 0,
               "VGA text buffer must be 16-bit aligned");

/* Standard 4-bit VGA palette indices. */
typedef enum {
    VGA_COLOR_BLACK         = 0x0,
    VGA_COLOR_BLUE          = 0x1,
    VGA_COLOR_GREEN         = 0x2,
    VGA_COLOR_CYAN          = 0x3,
    VGA_COLOR_RED           = 0x4,
    VGA_COLOR_MAGENTA       = 0x5,
    VGA_COLOR_BROWN         = 0x6,
    VGA_COLOR_LIGHT_GREY    = 0x7,
    VGA_COLOR_DARK_GREY     = 0x8,
    VGA_COLOR_LIGHT_BLUE    = 0x9,
    VGA_COLOR_LIGHT_GREEN   = 0xA,
    VGA_COLOR_LIGHT_CYAN    = 0xB,
    VGA_COLOR_LIGHT_RED     = 0xC,
    VGA_COLOR_LIGHT_MAGENTA = 0xD,
    VGA_COLOR_YELLOW        = 0xE,
    VGA_COLOR_WHITE         = 0xF,
} vga_color_t;

/*
 * The buffer is memory-mapped I/O: every access must actually reach the
 * hardware, so the pointer target is volatile. "const" on the pointer itself
 * prevents it from ever being reassigned.
 */
static volatile uint16_t *const vga_buffer =
    (volatile uint16_t *)VGA_TEXT_BUFFER_ADDR;

/* Pack foreground/background colours into a VGA attribute byte. */
static inline uint8_t vga_make_attr(vga_color_t fg, vga_color_t bg)
{
    return (uint8_t)(((uint8_t)bg & 0x0Fu) << 4) | ((uint8_t)fg & 0x0Fu);
}

/* Pack a character and attribute byte into one 16-bit VGA cell. */
static inline uint16_t vga_make_cell(uint8_t ch, uint8_t attr)
{
    return (uint16_t)((uint16_t)attr << 8) | (uint16_t)ch;
}

/*
 * Write one cell. Returns false (and writes nothing) if (row, col) is outside
 * the 80x25 screen, so a bad coordinate can never scribble past 0xB8FA0.
 */
static bool vga_put_at(uint8_t ch, uint8_t attr, size_t row, size_t col)
{
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return false;
    }
    vga_buffer[row * VGA_WIDTH + col] = vga_make_cell(ch, attr);
    return true;
}

/* Fill every cell of the screen with a blank in the given attribute. */
static void vga_clear_screen(uint8_t attr)
{
    const uint16_t blank = vga_make_cell((uint8_t)' ', attr);
    for (size_t i = 0; i < VGA_CELL_COUNT; i++) {
        vga_buffer[i] = blank;
    }
}

/*
 * Write a NUL-terminated string starting at (row, col). Output is clipped at
 * the end of the row (no wrapping), so the scan of `str` is bounded to at
 * most VGA_WIDTH characters even if the string is not terminated.
 * Returns the number of characters written.
 */
static size_t vga_write_at(const char *str, uint8_t attr, size_t row, size_t col)
{
    if (str == NULL || row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return 0;
    }

    size_t written = 0;
    while (col + written < VGA_WIDTH && str[written] != '\0') {
        (void)vga_put_at((uint8_t)str[written], attr, row, col + written);
        written++;
    }
    return written;
}

/* Write "0x" followed by exactly 16 hex digits. Returns characters written. */
static size_t vga_write_hex64_at(uint64_t value, uint8_t attr, size_t row, size_t col)
{
    /* Sized by the initializer (16 digits + NUL) so the terminator is kept;
       GCC 15 rejects a [16] array here under -Wunterminated-string-init. */
    static const char digits[] = "0123456789ABCDEF";
    _Static_assert(sizeof(digits) == 17, "16 hex digits plus NUL");
    char buf[2 + 16 + 1];

    buf[0] = '0';
    buf[1] = 'x';
    for (size_t i = 0; i < 16; i++) {
        const uint64_t shift = (uint64_t)(60u - 4u * i);
        buf[2 + i] = digits[(value >> shift) & 0xFu];
    }
    buf[sizeof(buf) - 1] = '\0';

    return vga_write_at(buf, attr, row, col);
}

/* ------------------------------------------------------------------------- */
/* Multiboot2 boot-information validation                                    */
/* ------------------------------------------------------------------------- */

/* Fixed header at the start of the Multiboot2 boot information structure. */
typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_header_t;

_Static_assert(sizeof(mb2_info_header_t) == 8, "MB2 info header is 8 bytes");
_Static_assert(offsetof(mb2_info_header_t, total_size) == 0, "MB2 layout");
_Static_assert(offsetof(mb2_info_header_t, reserved) == 4, "MB2 layout");

typedef enum {
    MB2_INFO_OK = 0,
    MB2_INFO_NULL,
    MB2_INFO_MISALIGNED,
    MB2_INFO_UNMAPPED,
    MB2_INFO_BAD_SIZE,
} mb2_info_status_t;

/*
 * Structurally validate the boot information pointer before anything
 * dereferences it: non-NULL, 8-byte aligned, and wholly inside the region
 * that boot.asm identity mapped. Reading it outside that region would page
 * fault, and with no IDT yet that is a triple fault (instant reboot).
 */
static mb2_info_status_t mb2_validate_info(uint64_t info_addr)
{
    if (info_addr == 0) {
        return MB2_INFO_NULL;
    }
    if ((info_addr % MB2_INFO_ALIGN) != 0) {
        return MB2_INFO_MISALIGNED;
    }
    /* Header itself must be mapped before we may read total_size. Written
       as a subtraction so the comparison cannot overflow. */
    if (info_addr > BOOT_IDENTITY_MAP_END - sizeof(mb2_info_header_t)) {
        return MB2_INFO_UNMAPPED;
    }

    const volatile mb2_info_header_t *hdr =
        (const volatile mb2_info_header_t *)(uintptr_t)info_addr;
    const uint32_t total_size = hdr->total_size;

    if (total_size < MB2_INFO_MIN_SIZE || (total_size % MB2_INFO_ALIGN) != 0) {
        return MB2_INFO_BAD_SIZE;
    }
    if ((uint64_t)total_size > BOOT_IDENTITY_MAP_END - info_addr) {
        return MB2_INFO_UNMAPPED;
    }
    return MB2_INFO_OK;
}

static const char *mb2_info_status_str(mb2_info_status_t status)
{
    switch (status) {
    case MB2_INFO_OK:         return "OK";
    case MB2_INFO_NULL:       return "NULL pointer";
    case MB2_INFO_MISALIGNED: return "not 8-byte aligned";
    case MB2_INFO_UNMAPPED:   return "outside 2 MiB identity map";
    case MB2_INFO_BAD_SIZE:   return "invalid total_size";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Kernel entry point                                                        */
/* ------------------------------------------------------------------------- */

/* Non-static: called from boot.asm. Declared here so the definition is
   checked against a prototype (-Wmissing-prototypes). */
__attribute__((noreturn))
void kernel_main(uint32_t mb2_magic, uint64_t mb2_info_addr);

__attribute__((noreturn))
void kernel_main(uint32_t mb2_magic, uint64_t mb2_info_addr)
{
    const uint8_t attr_normal = vga_make_attr(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    const uint8_t attr_ok     = vga_make_attr(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE);
    const uint8_t attr_error  = vga_make_attr(VGA_COLOR_WHITE, VGA_COLOR_RED);

    vga_clear_screen(attr_normal);

    (void)vga_write_at("AuraOS: Successfully booted into 64-bit Long Mode!",
                       attr_normal, 0, 0);

    /* boot.asm already checked the magic, but the C side must not trust its
       caller: re-check before interpreting RSI as a Multiboot2 pointer. */
    if (mb2_magic != MB2_BOOTLOADER_MAGIC) {
        size_t col = vga_write_at("Multiboot2 magic INVALID: ", attr_error, 2, 0);
        (void)vga_write_hex64_at((uint64_t)mb2_magic, attr_error, 2, col);
    } else {
        (void)vga_write_at("Multiboot2 magic OK", attr_ok, 2, 0);

        const mb2_info_status_t status = mb2_validate_info(mb2_info_addr);
        const uint8_t attr = (status == MB2_INFO_OK) ? attr_ok : attr_error;

        size_t col = vga_write_at("Multiboot2 info at ", attr, 3, 0);
        col += vga_write_hex64_at(mb2_info_addr, attr, 3, col);
        col += vga_write_at(": ", attr, 3, col);
        (void)vga_write_at(mb2_info_status_str(status), attr, 3, col);
    }

    /* Nothing left to do in Phase 1. Interrupts are disabled (IF = 0), so HLT
       sleeps until an NMI/SMI; the loop puts the CPU straight back to sleep. */
    while (1) {
        __asm__ volatile("hlt");
    }
}
