/*
 * AuraOS - linker.ld
 *
 * Lays out the kernel as a 64-bit ELF image loaded at physical 1 MiB.
 *
 *   0x00100000  .multiboot_header   (must be within the first 32 KiB, 8-aligned)
 *               .text               (32-bit .boot.text first, then 64-bit code)
 *               .rodata
 *               .data
 *               .bss                (page tables, boot stack, C globals)
 *   < 0x00200000                    (boot.asm identity maps only 0 - 2 MiB)
 *
 * Virtual address == physical address (identity mapping) in Phase 1.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Constants shared with boot.asm. Changing one requires changing the other. */
KERNEL_LOAD_ADDR      = 0x00100000;   /* 1 MiB: above BIOS/VGA/legacy areas  */
BOOT_IDENTITY_MAP_END = 0x00200000;   /* End of 2 MiB identity map in boot.asm */
MB2_HEADER_SEARCH_END = 32768;        /* Multiboot2 spec: header in first 32 KiB */

SECTIONS
{
    . = KERNEL_LOAD_ADDR;
    __kernel_start = .;

    /*
     * The Multiboot2 header comes first. KEEP() stops --gc-sections from
     * discarding it (nothing references it by symbol).
     */
    .multiboot_header ALIGN(8) :
    {
        __multiboot_header_start = .;
        KEEP(*(.multiboot_header))
        __multiboot_header_end = .;
    }

    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.boot.text)              /* 32-bit bootstrap from boot.asm */
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    /*
     * .bss is zeroed by boot.asm with "rep stosd", so both ends must be
     * 4-byte aligned. Page tables inside it request 4 KiB alignment themselves.
     */
    .bss ALIGN(4K) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __kernel_end = .;

    /*
     * Discard sections that are useless or harmful in a freestanding kernel.
     * .note.gnu.property in particular can be emitted ahead of our sections
     * and push the Multiboot2 header out of the 32 KiB search window.
     */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.interp)
        *(.dynamic .dynsym .dynstr .hash .gnu.hash)
    }
}

/* ---- Link-time sanity checks: the link fails if any of these are false ---- */
ASSERT(SIZEOF(.multiboot_header) > 0,
       "Multiboot2 header missing: is multiboot_header.o linked?")
ASSERT(__multiboot_header_start == KERNEL_LOAD_ADDR,
       "Multiboot2 header must be the first thing in the image")
ASSERT(__multiboot_header_end - KERNEL_LOAD_ADDR <= MB2_HEADER_SEARCH_END,
       "Multiboot2 header must lie within the first 32 KiB of the image")
ASSERT((__bss_start % 4) == 0 && (__bss_end % 4) == 0,
       ".bss bounds must be 4-byte aligned for rep stosd zeroing")
ASSERT(__kernel_end <= BOOT_IDENTITY_MAP_END,
       "Kernel exceeds the 2 MiB boot identity map; extend paging in boot.asm")
