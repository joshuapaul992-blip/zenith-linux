; =============================================================================
;  arch/x86_64/syscall_entry.asm  --  SYSCALL/SYSRET fast system-call path
;
;  Programmed into IA32_LSTAR by syscall_init(). On SYSCALL the CPU has:
;      RCX = user RIP,  R11 = user RFLAGS,  RSP = still the user stack
;      RAX = syscall number, RDI RSI RDX R10 R8 R9 = arguments (Linux ABI)
;  IA32_FMASK clears IF/DF/TF on entry.
;
;  SWAPGS makes GS this CPU's percpu block (smp.h): the user RSP is parked
;  at gs:16 and the current thread's kernel stack top read from gs:8. We
;  build a struct int_frame identical to the one isr_common builds (vector
;  0x80) and reuse the same C dispatcher as the `int 0x80` gate. On the way
;  out nothing is read through GS (the thread may have moved to another CPU
;  meanwhile); SWAPGS is the last instruction before SYSRET/IRETQ, with
;  interrupts off. An NMI in between is handled by the paranoid entry.
;
;  NOTE: SYSRET always returns to ring 3, so this path is only exercised by
;  user-mode code. Kernel threads use `int 0x80` (see usyscall.h).
; =============================================================================

bits 64
section .text

global syscall_entry
extern syscall_dispatch

syscall_entry:
    swapgs
    mov     [gs:16], rsp                ; percpu.user_rsp
    mov     rsp, [gs:8]                 ; percpu.kernel_rsp

    push    0x1B                        ; ss  (user data | RPL3)
    push    qword [gs:16]               ; rsp
    push    r11                         ; rflags
    push    0x23                        ; cs  (user code | RPL3)
    push    rcx                         ; rip
    push    0                           ; error code
    push    0x80                        ; vector

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

    mov     rdi, rsp
    sti
    call    syscall_dispatch            ; nonzero: return through iretq
    cli
    test    eax, eax
    jnz     .iret
    ; Never SYSRET to a non-canonical (or kernel) RIP: on Intel CPUs SYSRET
    ; would fault in ring 0 on the user stack. IRETQ faults safely in ring 3.
    mov     rcx, [rsp + 17*8]           ; frame.rip
    shr     rcx, 47
    jnz     .iret

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
    add     rsp, 16                     ; vector + error code
    pop     rcx                         ; user RIP    -> RCX for SYSRET
    add     rsp, 8                      ; cs
    pop     r11                         ; user RFLAGS -> R11 for SYSRET
    pop     rsp                         ; user RSP
    swapgs
    o64 sysret

    ; rt_sigreturn restores a full context, rcx and r11 included, which
    ; SYSRET would overwrite: leave through IRETQ (the frame above is laid
    ; out like an interrupt frame).
.iret:
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
    add     rsp, 16                     ; vector + error code
    swapgs
    iretq
