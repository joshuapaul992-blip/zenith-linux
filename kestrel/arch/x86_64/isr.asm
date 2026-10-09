; =============================================================================
;  arch/x86_64/isr.asm  --  interrupt / exception entry stubs
;
;  Every vector pushes (error code, vector number), then jumps to isr_common,
;  which saves all general-purpose registers and calls
;        void isr_dispatch(struct int_frame *frame);
;  The stack layout built here must match struct int_frame in idt.h.
;
;  Vectors installed:   0-31  CPU exceptions
;                      32-47  legacy PIC IRQs 0-15
;                        128  int 0x80 POSIX system-call gate (DPL 3)
; =============================================================================

bits 64
section .text

extern isr_dispatch
global isr_stub_table
global isr_syscall_stub

%macro ISR_NOERR 1
isr_stub_%1:
    push    0                           ; dummy error code
    push    %1                          ; vector
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push    %1                          ; CPU already pushed error code
    jmp     isr_common
%endmacro

; ---- CPU exceptions (vectors with an error code: 8,10-14,17,21,29,30) -------
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; ---- hardware IRQs (PIC remapped to 32-47) ----------------------------------
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; ---- int 0x80 system call ---------------------------------------------------
isr_syscall_stub:
    push    0
    push    0x80
    jmp     isr_common

; ---- common path ------------------------------------------------------------
isr_common:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rdi, rsp                    ; struct int_frame *
    cld                                 ; SysV ABI: DF clear on call
    call    isr_dispatch

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16                     ; drop vector + error code
    iretq

; ---- table of stub addresses for idt.c --------------------------------------
section .rodata
align 8
isr_stub_table:
%assign i 0
%rep 48
    dq isr_stub_ %+ i
%assign i i+1
%endrep
