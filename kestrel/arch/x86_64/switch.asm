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

; -----------------------------------------------------------------------------
; void enter_user(uint64_t rip, uint64_t rsp)   -- noreturn
;
; First entry of a user process into ring 3 (proc/exec.c). Builds an iretq
; frame (SS=user data, CS=user code, IF set) and clears every general
; register so no kernel value leaks into the process. FS base (TLS) is a
; per-task MSR value restored by the scheduler, so FS itself is not loaded.
; -----------------------------------------------------------------------------
global enter_user
enter_user:
    cli
    mov     ax, 0x1B                    ; user data | RPL3
    mov     ds, ax
    mov     es, ax
    push    0x1B                        ; ss
    push    rsi                         ; rsp
    push    0x202                       ; rflags: IF
    push    0x23                        ; cs: user code | RPL3
    push    rdi                         ; rip
    xor     eax, eax
    xor     ebx, ebx
    xor     ecx, ecx
    xor     edx, edx
    xor     esi, esi
    xor     edi, edi
    xor     ebp, ebp
    xor     r8d, r8d
    xor     r9d, r9d
    xor     r10d, r10d
    xor     r11d, r11d
    xor     r12d, r12d
    xor     r13d, r13d
    xor     r14d, r14d
    xor     r15d, r15d
    iretq
