; =============================================================================
;  arch/x86_64/isr.asm  --  interrupt / exception entry stubs
;
;  Every vector pushes (error code, vector number), then jumps to a common
;  path that saves all general-purpose registers and calls
;        void isr_dispatch(struct int_frame *frame);
;  The stack layout built here must match struct int_frame in arch.h.
;
;  GS: in the kernel, the GS base is this CPU's percpu block (smp.h). An
;  entry from ring 3 executes SWAPGS (and the return to ring 3 swaps back).
;  NMI (2), #DF (8) and #MC (18) can arrive anywhere -- also between a
;  SYSCALL and its SWAPGS -- so they take the paranoid path: read the GS
;  base and swap only if it is not a percpu block.
;
;  Vectors:  0-31  CPU exceptions
;           32-47  ISA IRQs 0-15 (8259 PIC or IO APIC)
;             128  int 0x80 POSIX system-call gate (DPL 3)
;         224-255  IPIs, local APIC timer, spurious (smp.h)
;  All 256 vectors have a stub; unexpected ones are counted and ignored.
; =============================================================================

bits 64
section .text

extern isr_dispatch
extern percpu_is_kernel_gs
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

%macro ISR_PARANOID 2                   ; vector, has error code
isr_stub_%1:
%if %2 == 0
    push    0
%endif
    push    %1
    jmp     isr_paranoid
%endmacro

; ---- CPU exceptions (vectors with an error code: 8,10-14,17,21,29,30) -------
ISR_NOERR 0
ISR_NOERR 1
ISR_PARANOID 2, 0
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_PARANOID 8, 1
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_PARANOID 18, 0
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

; ---- everything else: IRQs, IPIs, timer, spurious ----------------------------
%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

; ---- int 0x80 system call (its own gate: DPL 3) ------------------------------
isr_syscall_stub:
    push    0
    push    0x80
    jmp     isr_common

%macro PUSH_REGS 0
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
%endmacro

%macro POP_REGS 0
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
%endmacro

; ---- common path: [rsp] vector, +8 error, +16 rip, +24 cs ---------------------
isr_common:
    test    qword [rsp + 24], 3
    jz      .from_kernel
    swapgs                              ; from ring 3: kernel GS
.from_kernel:
    PUSH_REGS
    mov     rdi, rsp                    ; struct int_frame *
    cld                                 ; SysV ABI: DF clear on call
    call    isr_dispatch
    POP_REGS
    test    qword [rsp + 24], 3
    jz      .to_kernel
    swapgs                              ; back to ring 3: user GS
.to_kernel:
    add     rsp, 16                     ; drop vector + error code
    iretq

; ---- paranoid path (NMI, #DF, #MC): decide by the GS base itself --------------
isr_paranoid:
    PUSH_REGS
    mov     ecx, 0xC0000101             ; IA32_GS_BASE
    rdmsr
    shl     rdx, 32
    or      rax, rdx
    mov     rdi, rax
    cld
    call    percpu_is_kernel_gs         ; uses no GS
    xor     ebx, ebx                    ; rbx: swapped? (callee-saved)
    test    al, al
    jnz     .kernel_gs
    swapgs
    mov     ebx, 1
.kernel_gs:
    mov     rdi, rsp
    call    isr_dispatch
    test    ebx, ebx
    jz      .noswap
    swapgs
.noswap:
    POP_REGS
    add     rsp, 16
    iretq

; ---- table of stub addresses for idt.c --------------------------------------
section .rodata
align 8
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_ %+ i
%assign i i+1
%endrep
