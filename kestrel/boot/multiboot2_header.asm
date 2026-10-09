; =============================================================================
;  boot/multiboot2_header.asm  --  Multiboot2 header for Kestrel
;
;  The header asks the boot loader for a 1024x768x32 linear frame buffer.
;  A Multiboot2 loader (GRUB 2, Limine, ...) satisfies this request with
;    * VBE (VESA BIOS Extensions) when booted from legacy BIOS, or
;    * GOP (UEFI Graphics Output Protocol) when booted from UEFI firmware,
;  and passes the resulting mode back in the "framebuffer info" tag (type 8).
;  The kernel therefore never has to call VBE/GOP itself.
;
;  The header must live in the first 32 KiB of the image, 8-byte aligned.
;  The linker script places section .multiboot2 at the very start of .text.
; =============================================================================

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0                       ; 32-bit protected-mode entry
MB2_LENGTH      equ mb2_header_end - mb2_header_start
MB2_CHECKSUM    equ 0x100000000 - (MB2_MAGIC + MB2_ARCH_I386 + MB2_LENGTH)

; Requested video mode (also exported to the kernel for sanity checks)
FB_WIDTH        equ 1024
FB_HEIGHT       equ 768
FB_DEPTH        equ 32

section .multiboot2
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd MB2_LENGTH
    dd MB2_CHECKSUM & 0xFFFFFFFF

    ; ---- Tag 1: information request ----------------------------------------
    ; Ask for: boot command line (1), loader name (2), memory map (6),
    ; framebuffer info (8). All are always provided by GRUB 2.
align 8
.info_req_start:
    dw 1                                    ; type  = information request
    dw 0                                    ; flags = mandatory
    dd .info_req_end - .info_req_start      ; size
    dd 1                                    ; MULTIBOOT_TAG_TYPE_CMDLINE
    dd 2                                    ; MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME
    dd 6                                    ; MULTIBOOT_TAG_TYPE_MMAP
    dd 8                                    ; MULTIBOOT_TAG_TYPE_FRAMEBUFFER
.info_req_end:

    ; ---- Tag 5: framebuffer --------------------------------------------------
    ; flags = 0 -> the loader MUST give us a graphics mode (no text fallback).
align 8
.fb_start:
    dw 5                                    ; type  = framebuffer
    dw 0                                    ; flags = mandatory
    dd .fb_end - .fb_start                  ; size  = 20
    dd FB_WIDTH
    dd FB_HEIGHT
    dd FB_DEPTH
.fb_end:

    ; ---- Tag 0: end ---------------------------------------------------------
align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:
