; =============================================================================
;  boot/boot.asm  --  32-bit Multiboot2 entry -> 64-bit long mode bootstrap
;
;  Machine state on entry (Multiboot2 spec, i386 entry):
;     EAX = 0x36D76289 (Multiboot2 bootloader magic)
;     EBX = physical address of the Multiboot2 information structure
;     32-bit protected mode, paging off, interrupts off, A20 on.
;
;  What this file does:
;     1. sets up a temporary stack
;     2. verifies the magic, CPUID and long-mode support
;     3. builds identity-mapped page tables for the first 4 GiB using
;        2 MiB pages (covers the kernel, low RAM and the VBE/GOP frame
;        buffer, which firmware normally places below 4 GiB)
;     4. enables PAE + EFER.LME + paging, loads a minimal 64-bit GDT
;     5. far-jumps into 64-bit code and calls kernel_main(magic, mbi)
;
;  Any failure before long mode prints a short message on COM1 and halts.
; =============================================================================

MB2_BOOTLOADER_MAGIC equ 0x36D76289
BOOT_STACK_SIZE      equ 32768

global _start
global boot_pml4
global boot_stack
global boot_stack_top
extern kernel_main

; -----------------------------------------------------------------------------
section .bss
align 4096
boot_pml4:      resb 4096
boot_pdpt:      resb 4096
boot_pd:        resb 4096 * 4           ; 4 page directories -> 4 GiB
align 16
boot_stack:     resb BOOT_STACK_SIZE
boot_stack_top:

; -----------------------------------------------------------------------------
section .rodata
align 16
gdt64:
    dq 0                                ; 0x00 null
    dq 0x00209A0000000000               ; 0x08 kernel code: P, DPL0, exec, L=1
    dq 0x0000920000000000               ; 0x10 kernel data: P, DPL0, writable
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

msg_no_mb:  db "Kestrel: not booted by a Multiboot2 loader", 13, 10, 0
msg_no_cpu: db "Kestrel: CPUID not supported", 13, 10, 0
msg_no_lm:  db "Kestrel: CPU is not x86_64 (no long mode)", 13, 10, 0

; -----------------------------------------------------------------------------
section .text
bits 32

_start:
    cli
    cld
    mov     esp, boot_stack_top
    mov     edi, eax                    ; keep magic in EDI
    mov     esi, ebx                    ; keep MBI pointer in ESI

    ; Reset EFLAGS to a known state
    push    0
    popfd

    ; ---- 1. Multiboot2 magic ----------------------------------------------
    cmp     edi, MB2_BOOTLOADER_MAGIC
    jne     .err_no_mb

    ; ---- 2. CPUID available? (toggle EFLAGS.ID, bit 21) ---------------------
    pushfd
    pop     eax
    mov     ecx, eax
    xor     eax, 1 << 21
    push    eax
    popfd
    pushfd
    pop     eax
    push    ecx
    popfd
    cmp     eax, ecx
    je      .err_no_cpuid

    ; ---- 3. Long mode available? -------------------------------------------
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      .err_no_lm
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29                ; LM bit
    jz      .err_no_lm

    ; ---- 4. Build page tables ----------------------------------------------
    ; zero PML4 + PDPT + 4 PDs (6 pages)
    push    edi
    mov     edi, boot_pml4
    xor     eax, eax
    mov     ecx, (4096 * 6) / 4
    rep     stosd
    pop     edi

    ; PML4[0] -> PDPT
    mov     eax, boot_pdpt
    or      eax, 0x003                  ; present | writable
    mov     [boot_pml4], eax

    ; PDPT[0..3] -> PD0..PD3
    mov     eax, boot_pd
    or      eax, 0x003
    mov     ecx, 0
.fill_pdpt:
    mov     [boot_pdpt + ecx * 8], eax
    add     eax, 4096
    inc     ecx
    cmp     ecx, 4
    jne     .fill_pdpt

    ; PD entries: 2048 x 2 MiB identity pages (present|writable|PS)
    xor     ecx, ecx
.fill_pd:
    mov     eax, ecx
    shl     eax, 21
    or      eax, 0x083
    mov     [boot_pd + ecx * 8], eax
    mov     dword [boot_pd + ecx * 8 + 4], 0
    inc     ecx
    cmp     ecx, 2048
    jne     .fill_pd

    ; ---- 5. Enter long mode -------------------------------------------------
    mov     eax, cr4
    or      eax, 1 << 5                 ; CR4.PAE
    mov     cr4, eax

    mov     eax, boot_pml4
    mov     cr3, eax

    mov     ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or      eax, 1 << 8                 ; EFER.LME
    wrmsr

    mov     eax, cr0
    or      eax, (1 << 31) | (1 << 16) | 1   ; PG | WP | PE
    mov     cr0, eax

    lgdt    [gdt64_ptr]
    jmp     0x08:long_mode_entry

; ---- error paths: print to COM1 then halt -----------------------------------
.err_no_mb:
    mov     ebx, msg_no_mb
    jmp     early_panic
.err_no_cpuid:
    mov     ebx, msg_no_cpu
    jmp     early_panic
.err_no_lm:
    mov     ebx, msg_no_lm
    jmp     early_panic

; EBX = NUL-terminated string
early_panic:
    mov     dx, 0x3F8
.next:
    mov     al, [ebx]
    test    al, al
    jz      .halt
    push    edx
    mov     dx, 0x3FD                   ; line status register
.wait:
    in      al, dx
    test    al, 0x20                    ; THR empty?
    jz      .wait
    pop     edx
    mov     al, [ebx]
    out     dx, al
    inc     ebx
    jmp     .next
.halt:
    cli
    hlt
    jmp     .halt

; -----------------------------------------------------------------------------
bits 64
long_mode_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    xor     ax, ax
    mov     fs, ax
    mov     gs, ax

    mov     rsp, boot_stack_top
    xor     rbp, rbp                    ; terminate stack-frame chain

    ; upper halves of 64-bit registers are undefined after the mode switch
    mov     edi, edi                    ; arg0: multiboot magic (zero-extended)
    mov     esi, esi                    ; arg1: MBI physical address
    call    kernel_main

.hang:
    cli
    hlt
    jmp     .hang
