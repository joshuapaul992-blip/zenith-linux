; =============================================================================
; AuraOS - multiboot_header.asm
;
; Multiboot2 header (Multiboot Specification version 2.0, section 3.1).
;
; GRUB scans the first 32 KiB of the kernel image for this header. The header
; must be 64-bit (8-byte) aligned, and every tag inside it must also start on
; an 8-byte boundary. The linker script places this section first in the image
; and asserts that it sits inside that 32 KiB window.
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Header constants
; -----------------------------------------------------------------------------
MB2_HEADER_MAGIC        equ 0xE85250D6  ; Magic value that GRUB searches for
MB2_ARCH_I386           equ 0           ; 0 = 32-bit protected mode on i386

; Tag types (spec section 3.1.3 onwards)
MB2_TAG_END             equ 0
MB2_TAG_INFO_REQUEST    equ 1
MB2_TAG_FRAMEBUFFER     equ 5

; Tag flags. Bit 0 set = "optional": the bootloader may ignore the tag if it
; does not support it, instead of refusing to boot.
MB2_TAG_FLAG_REQUIRED   equ 0
MB2_TAG_FLAG_OPTIONAL   equ 1

; Boot information tag types that we ask GRUB to supply (spec section 3.6).
MB2_INFO_CMDLINE        equ 1
MB2_INFO_BASIC_MEMINFO  equ 4
MB2_INFO_MMAP           equ 6
MB2_INFO_FRAMEBUFFER    equ 8

section .multiboot_header
align 8

mb2_header_start:
    dd MB2_HEADER_MAGIC                                     ; magic
    dd MB2_ARCH_I386                                        ; architecture
    dd mb2_header_end - mb2_header_start                    ; header_length
    ; checksum: magic + architecture + header_length + checksum == 0 (mod 2^32)
    dd 0x100000000 - (MB2_HEADER_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    ; -------------------------------------------------------------------------
    ; Information request tag
    ;
    ; Asks GRUB to include these tags in the boot information structure it
    ; passes to us in EBX. Marked optional so that a firmware environment that
    ; cannot supply one of them (e.g. basic meminfo on some EFI systems) still
    ; boots; the kernel must check that each tag is present before using it.
    ; -------------------------------------------------------------------------
align 8
mb2_tag_info_request_start:
    dw MB2_TAG_INFO_REQUEST                                 ; type
    dw MB2_TAG_FLAG_OPTIONAL                                ; flags
    dd mb2_tag_info_request_end - mb2_tag_info_request_start ; size
    dd MB2_INFO_CMDLINE
    dd MB2_INFO_BASIC_MEMINFO
    dd MB2_INFO_MMAP
    dd MB2_INFO_FRAMEBUFFER
mb2_tag_info_request_end:

    ; -------------------------------------------------------------------------
    ; Framebuffer tag
    ;
    ; Width 80, height 25, depth 0 states our preference for an 80x25 text
    ; console (depth 0 = text in the spec). Phase 1 writes straight to the VGA
    ; text buffer at 0xB8000, which is invisible in a graphics mode.
    ;
    ; CAUTION: GRUB treats the mere presence of this tag as "kernel accepts a
    ; linear framebuffer" and turns the request into "80x25,auto", which on
    ; BIOS selects a VBE graphics mode. iso/boot/grub/grub.cfg therefore sets
    ; gfxpayload=text after the multiboot2 command. The tag is optional so
    ; GRUB still boots if it cannot honour it (e.g. UEFI GOP-only systems).
    ; -------------------------------------------------------------------------
align 8
mb2_tag_framebuffer_start:
    dw MB2_TAG_FRAMEBUFFER                                  ; type
    dw MB2_TAG_FLAG_OPTIONAL                                ; flags
    dd mb2_tag_framebuffer_end - mb2_tag_framebuffer_start  ; size (= 20)
    dd 80                                                   ; width  (columns)
    dd 25                                                   ; height (rows)
    dd 0                                                    ; depth  (0 = text)
mb2_tag_framebuffer_end:

    ; -------------------------------------------------------------------------
    ; End tag: terminates the tag list. Must be 8-byte aligned, size 8.
    ; -------------------------------------------------------------------------
align 8
mb2_tag_end_start:
    dw MB2_TAG_END                                          ; type
    dw 0                                                    ; flags
    dd mb2_tag_end_end - mb2_tag_end_start                  ; size (= 8)
mb2_tag_end_end:

mb2_header_end:

; -----------------------------------------------------------------------------
; Assemble-time sanity checks.
;
; NASM's preprocessor (%if) cannot see label values, so we use the
; "negative TIMES" idiom instead: when the condition is true the expression is
; -1 and NASM aborts with "TIMES value -1 is negative"; when false it emits
; zero bytes. These lines sit after mb2_header_end so they never add bytes to
; the header.
; -----------------------------------------------------------------------------
%define MB2_STATIC_ASSERT(cond) times -(!(cond)) db 0

MB2_STATIC_ASSERT((mb2_tag_info_request_end - mb2_tag_info_request_start) == 24)
MB2_STATIC_ASSERT((mb2_tag_framebuffer_end - mb2_tag_framebuffer_start) == 20)
MB2_STATIC_ASSERT((mb2_tag_end_end - mb2_tag_end_start) == 8)
MB2_STATIC_ASSERT(((mb2_header_end - mb2_header_start) % 8) == 0)
