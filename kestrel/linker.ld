/*
 * linker.ld -- Kestrel kernel layout
 *
 * The kernel is linked and loaded at 2 MiB physical (identity mapped by
 * boot.asm). 2 MiB rather than the traditional 1 MiB keeps clear of memory
 * that UEFI firmware commonly reserves below 2 MiB, so the same ELF boots
 * from GRUB on both BIOS and UEFI.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS_BASE = 0x200000;

SECTIONS
{
    . = KERNEL_PHYS_BASE;
    _kernel_start = .;

    .text ALIGN(4K) : {
        KEEP(*(.multiboot2))        /* must be within the first 32 KiB */
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    /* <faulting RIP, fixup RIP> pairs for user-memory access (uaccess.asm) */
    .ex_table ALIGN(8) : {
        __ex_table_start = .;
        KEEP(*(.ex_table))
        __ex_table_end = .;
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : {              /* zero-filled by the ELF loader */
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    }

    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note .note.*)
    }
}
