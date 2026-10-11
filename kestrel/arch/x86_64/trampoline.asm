; =============================================================================
;  arch/x86_64/trampoline.asm  --  start-up code of the other processors
;
;  An application processor (AP) starts in 16-bit real mode at the page the
;  STARTUP IPI names. smp.c copies this blob there (a usable page below
;  640 KiB) and patches its data block at the end: the kernel's page tables
;  (CR3, below 4 GiB), the boot CPU's EFER, a stack, the C entry point and
;  its argument (the CPU's percpu block). Absolute addresses are computed
;  from TRAMP_BASE: the blob is assembled for that page only, and smp.c
;  starts no AP if the page is not usable RAM.
;
;      real mode -> 32-bit protected mode (temporary GDT) -> PAE + LME +
;      paging -> 64-bit long mode -> ap_entry(percpu)
; =============================================================================

TRAMP_BASE equ 0x8000
%define T(x) (TRAMP_BASE + (x) - tramp_start)

global tramp_start, tramp_end, tramp_cr3, tramp_efer, tramp_stack, tramp_entry, tramp_arg

section .text
bits 16
tramp_start:
    cli
    cld
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    lgdt    [T(tramp_gdt_ptr)]
    mov     eax, cr0
    or      eax, 1                      ; PE
    mov     cr0, eax
    jmp     dword 0x08:T(tramp_pm)

bits 32
tramp_pm:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     eax, cr4
    or      eax, 1 << 5                 ; PAE
    mov     cr4, eax
    mov     eax, [T(tramp_cr3)]
    mov     cr3, eax
    mov     ecx, 0xC0000080             ; EFER: the boot CPU's (LME, NXE, SCE)
    mov     eax, [T(tramp_efer)]
    xor     edx, edx
    wrmsr
    mov     eax, cr0
    or      eax, (1 << 31) | (1 << 16)  ; PG | WP
    mov     cr0, eax
    jmp     0x18:T(tramp_lm)

bits 64
tramp_lm:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     rsp, [T(tramp_stack)]
    mov     rdi, [T(tramp_arg)]
    mov     rax, [T(tramp_entry)]
    xor     ebp, ebp
    call    rax                         ; ap_entry: never returns
.hang:
    cli
    hlt
    jmp     .hang

align 8
tramp_gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10 data
    dq 0x00AF9A000000FFFF               ; 0x18 64-bit code
tramp_gdt_ptr:
    dw 4 * 8 - 1
    dd T(tramp_gdt)
align 8
tramp_cr3:      dq 0
tramp_efer:     dq 0
tramp_stack:    dq 0
tramp_entry:    dq 0
tramp_arg:      dq 0
tramp_end:
