; =============================================================================
;  arch/x86_64/uaccess.asm  --  fault-tolerant access to user memory
;
;  Every instruction here that touches a user address is listed in the
;  exception table (section .ex_table: pairs of <faulting RIP, fixup RIP>).
;  When such an instruction faults, the page-fault handler (idt.c) first
;  tries to resolve the fault (demand paging, copy-on-write); if it cannot,
;  it resumes at the fixup address instead of treating the fault as a kernel
;  bug. The C wrappers in kernel/uaccess.c check the address range first, so
;  only user addresses ever reach these routines.
;
;  SysV ABI: rdi, rsi, rdx, rcx = arguments, rax = result.
; =============================================================================

bits 64
section .text

%macro EXTABLE 2
    section .ex_table
    align 8
    dq %1, %2
    section .text
%endmacro

; -----------------------------------------------------------------------------
; size_t uaccess_copy(void *dst, const void *src, size_t n)
; Returns the number of bytes NOT copied (0 on success). One of dst/src is a
; user address. `rep movsb` leaves rcx = bytes left when it faults.
; -----------------------------------------------------------------------------
global uaccess_copy
uaccess_copy:
    mov     rcx, rdx
.copy:
    rep movsb
    xor     eax, eax
    ret
.fault:
    mov     rax, rcx
    ret
    EXTABLE uaccess_copy.copy, uaccess_copy.fault

; -----------------------------------------------------------------------------
; size_t uaccess_clear(void *dst, size_t n): zero user memory; bytes NOT cleared
; -----------------------------------------------------------------------------
global uaccess_clear
uaccess_clear:
    mov     rcx, rsi
    xor     eax, eax
.store:
    rep stosb
    ret                                 ; rax = 0
.fault:
    mov     rax, rcx
    ret
    EXTABLE uaccess_clear.store, uaccess_clear.fault

; -----------------------------------------------------------------------------
; int64_t uaccess_strncpy(char *dst, const char *usrc, size_t max)
; Copies up to and including the NUL. Returns the string length (without the
; NUL), `max` if no NUL was found in the first max bytes (dst then holds max
; bytes, unterminated), or -1 on a fault.
; -----------------------------------------------------------------------------
global uaccess_strncpy
uaccess_strncpy:
    xor     eax, eax
.loop:
    cmp     rax, rdx
    jae     .done
.load:
    movzx   ecx, byte [rsi + rax]
    mov     [rdi + rax], cl
    test    cl, cl
    jz      .done
    inc     rax
    jmp     .loop
.done:
    ret
.fault:
    mov     rax, -1
    ret
    EXTABLE uaccess_strncpy.load, uaccess_strncpy.fault

; -----------------------------------------------------------------------------
; int uaccess_get32(const uint32_t *uaddr, uint32_t *out)   0 or -1
; int uaccess_get64(const uint64_t *uaddr, uint64_t *out)
; int uaccess_put32(uint32_t *uaddr, uint32_t v)
; int uaccess_put64(uint64_t *uaddr, uint64_t v)
; -----------------------------------------------------------------------------
global uaccess_get32
uaccess_get32:
.ld:
    mov     eax, [rdi]
    mov     [rsi], eax
    xor     eax, eax
    ret
.fault:
    mov     eax, -1
    ret
    EXTABLE uaccess_get32.ld, uaccess_get32.fault

global uaccess_get64
uaccess_get64:
.ld:
    mov     rax, [rdi]
    mov     [rsi], rax
    xor     eax, eax
    ret
.fault:
    mov     eax, -1
    ret
    EXTABLE uaccess_get64.ld, uaccess_get64.fault

global uaccess_put32
uaccess_put32:
.st:
    mov     [rdi], esi
    xor     eax, eax
    ret
.fault:
    mov     eax, -1
    ret
    EXTABLE uaccess_put32.st, uaccess_put32.fault

global uaccess_put64
uaccess_put64:
.st:
    mov     [rdi], rsi
    xor     eax, eax
    ret
.fault:
    mov     eax, -1
    ret
    EXTABLE uaccess_put64.st, uaccess_put64.fault

; -----------------------------------------------------------------------------
; int uaccess_cmpxchg32(uint32_t *uaddr, uint32_t old, uint32_t new, uint32_t *cur)
; Atomic compare-and-exchange on user memory (futexes, robust lists).
; *cur = the value found. Returns 0 (whether or not it matched) or -1 on a
; fault. `lock cmpxchg` is a full barrier on x86.
; -----------------------------------------------------------------------------
global uaccess_cmpxchg32
uaccess_cmpxchg32:
    mov     eax, esi
.op:
    lock cmpxchg [rdi], edx
    mov     [rcx], eax
    xor     eax, eax
    ret
.fault:
    mov     eax, -1
    ret
    EXTABLE uaccess_cmpxchg32.op, uaccess_cmpxchg32.fault
