; =============================================================================
;  arch/x86_64/switch.asm  --  kernel thread context switch + thread start
; =============================================================================

bits 64
section .text

global context_switch
global thread_trampoline
extern task_exit

; -----------------------------------------------------------------------------
; void context_switch(uint64_t *save_rsp, uint64_t load_rsp);
;
; Saves the callee-saved registers and RFLAGS of the current thread on its
; own kernel stack, stores RSP into *save_rsp, switches to load_rsp and
; restores the next thread's registers. Caller-saved registers are already
; preserved by the C calling convention.
; Saved frame (low -> high): r15 r14 r13 r12 rbp rbx rflags return-rip
; -----------------------------------------------------------------------------
context_switch:
    pushfq
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    mov     [rdi], rsp
    mov     rsp, rsi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    popfq
    ret

; -----------------------------------------------------------------------------
; First code run by a brand-new kernel thread. task_create() builds a fake
; context_switch frame whose return address points here, with
;     rbx = entry function,  r12 = argument.
; New threads begin with interrupts disabled (we were switched to from inside
; the scheduler), so enable them before running the thread body.
; -----------------------------------------------------------------------------
thread_trampoline:
    sti
    mov     rdi, r12
    call    rbx
    mov     edi, eax                    ; exit status = return value
    call    task_exit                   ; never returns
.spin:
    hlt
    jmp     .spin
