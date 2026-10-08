; =============================================================================
; AuraOS - boot.asm
;
; 32-bit protected-mode entry point (_start) -> 64-bit long mode -> kernel_main
;
; Machine state on entry (Multiboot2 spec, section 3.3, i386 machine state):
;   EAX    = 0x36D76289 (Multiboot2 bootloader magic)
;   EBX    = 32-bit physical address of the Multiboot2 boot information struct
;   CS     = 32-bit read/execute code segment, offset 0, limit 0xFFFFFFFF
;   DS..SS = 32-bit read/write data segments, offset 0, limit 0xFFFFFFFF
;   CR0    = PG cleared, PE set
;   EFLAGS = VM cleared, IF cleared
;   ESP    = UNDEFINED -> we must set up our own stack before any push/call
;   GDTR/IDTR = UNDEFINED -> we must load our own GDT before reloading segments
;
; Boot sequence:
;   1. Set up a 16 KiB, 16-byte-aligned stack.
;   2. Validate the Multiboot2 magic and save the boot information pointer.
;   3. Zero .bss (page tables, stack, C globals) - never trust leftover RAM.
;   4. Verify CPUID is available, then verify long mode via CPUID 0x80000001.
;   5. Build PML4 -> PDPT -> PD -> PT, identity mapping 0x0 - 0x1FFFFF
;      with 512 x 4 KiB pages.
;   6. Enable PAE (CR4), load CR3, set EFER.LME, enable paging (CR0.PG).
;      The CPU is now in IA-32e compatibility mode.
;   7. lgdt a GDT with 64-bit code and data descriptors, far jump into the
;      64-bit code segment. The CPU is now in 64-bit long mode.
;   8. Reload data segments, align the stack, and call kernel_main following
;      the System V AMD64 ABI: RDI = magic, RSI = boot info pointer.
;
; If any check fails we print "AuraOS boot error: X" to the VGA text buffer
; (X = single-character error code) and halt forever.
; =============================================================================

; -----------------------------------------------------------------------------
; Constants
; -----------------------------------------------------------------------------
MB2_BOOTLOADER_MAGIC    equ 0x36D76289

STACK_SIZE              equ 16384       ; 16 KiB boot stack

; Paging structure entry flags (Intel SDM Vol. 3A, section 4.5)
PAGE_PRESENT            equ (1 << 0)
PAGE_WRITABLE           equ (1 << 1)
PAGE_TABLE_ENTRIES      equ 512
PAGE_SIZE               equ 4096

; Control register / MSR bits
CR0_PE                  equ (1 << 0)    ; Protection Enable (already set by GRUB)
CR0_PG                  equ (1 << 31)   ; Paging
CR4_PAE                 equ (1 << 5)    ; Physical Address Extension
MSR_EFER                equ 0xC0000080  ; Extended Feature Enable Register
EFER_LME                equ (1 << 8)    ; Long Mode Enable

; CPUID
EFLAGS_ID               equ (1 << 21)   ; Toggleable iff CPUID is supported
CPUID_EXT_MAX           equ 0x80000000
CPUID_EXT_FEATURES      equ 0x80000001
CPUID_EXT_LM            equ (1 << 29)   ; EDX bit 29: Long Mode (Intel 64 / AMD64)

; GDT selectors (index * 8, TI = 0, RPL = 0)
GDT_KERNEL_CODE_SEL     equ 0x08
GDT_KERNEL_DATA_SEL     equ 0x10

; VGA text buffer for early error messages
VGA_TEXT_BUFFER         equ 0xB8000
VGA_TEXT_WIDTH          equ 80          ; Cells per row
VGA_ERROR_ATTR          equ 0x4F        ; White on red

; Error codes shown by boot_error
ERR_BAD_MAGIC           equ '0'
ERR_NO_CPUID            equ '1'
ERR_NO_LONG_MODE        equ '2'

; Symbols provided by the linker script / C code
extern __bss_start
extern __bss_end
extern kernel_main

global _start

; =============================================================================
; 32-bit protected-mode code
; =============================================================================
section .boot.text progbits alloc exec nowrite align=16
bits 32

_start:
    cli                                 ; GRUB guarantees IF=0; be explicit.
    cld                                 ; String ops must count upward.

    ; Use our own stack immediately: ESP is undefined on Multiboot2 entry.
    mov esp, boot_stack_top

    ; --- Step 2: validate the Multiboot2 magic ------------------------------
    ; Check it before touching EAX/EBX for anything else; if we were not
    ; booted by a Multiboot2 loader, EBX is garbage and must not be used.
    cmp eax, MB2_BOOTLOADER_MAGIC
    jne .bad_magic

    ; EDI/ESI are not used by anything below until the 64-bit hand-off,
    ; and by then their upper halves are guaranteed zero (see long_mode_start).
    mov edi, eax                        ; EDI = magic   -> becomes RDI (arg 1)
    mov esi, ebx                        ; ESI = MBI ptr -> becomes RSI (arg 2)

    ; --- Step 3: zero .bss --------------------------------------------------
    ; GRUB's ELF loader zero-fills bss, but we must not rely on it: the page
    ; tables live here and a stray bit would map garbage. The stack is also in
    ; .bss, which is safe because nothing has been pushed yet. Done inline
    ; (not as a call) precisely so no return address is on the stack being
    ; wiped. The linker script guarantees both symbols are 4-byte aligned.
    mov edx, edi                        ; preserve magic across rep stosd
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2                          ; byte count -> dword count
    xor eax, eax
    rep stosd
    mov edi, edx                        ; restore EDI = magic

    ; --- Step 4: CPU feature checks ------------------------------------------
    call check_cpuid
    call check_long_mode

    ; --- Steps 5 and 6: paging, then switch to long mode --------------------
    call setup_page_tables
    call enable_long_mode

    ; --- Step 7: load the 64-bit GDT and far jump ---------------------------
    lgdt [gdt64.pointer]
    jmp GDT_KERNEL_CODE_SEL:long_mode_start

.bad_magic:
    mov al, ERR_BAD_MAGIC
    jmp boot_error

; -----------------------------------------------------------------------------
; check_cpuid: CPUID exists iff software can flip EFLAGS.ID (bit 21).
; -----------------------------------------------------------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax                        ; ECX = original EFLAGS
    xor eax, EFLAGS_ID                  ; flip ID
    push eax
    popfd
    pushfd
    pop eax                             ; EAX = EFLAGS after attempted flip
    push ecx
    popfd                               ; restore original EFLAGS
    cmp eax, ecx
    je .no_cpuid                        ; bit did not change -> no CPUID
    ret
.no_cpuid:
    mov al, ERR_NO_CPUID
    jmp boot_error

; -----------------------------------------------------------------------------
; check_long_mode: extended leaf 0x80000001 must exist and report EDX.LM.
; CPUID clobbers EBX/ECX/EDX, none of which carry live state here.
; -----------------------------------------------------------------------------
check_long_mode:
    mov eax, CPUID_EXT_MAX
    cpuid
    cmp eax, CPUID_EXT_FEATURES
    jb .no_long_mode                    ; extended leaf 0x80000001 not present

    mov eax, CPUID_EXT_FEATURES
    cpuid
    test edx, CPUID_EXT_LM
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, ERR_NO_LONG_MODE
    jmp boot_error

; -----------------------------------------------------------------------------
; setup_page_tables: identity map physical 0x000000 - 0x1FFFFF (2 MiB).
;
;   PML4[0] -> PDPT
;   PDPT[0] -> PD
;   PD[0]   -> PT
;   PT[i]   =  (i * 4 KiB) | PRESENT | WRITABLE,   i = 0..511
;
; This covers the VGA buffer (0xB8000), the kernel (linked at 1 MiB, the
; linker script asserts it ends below 2 MiB) and the boot stack/page tables
; in .bss. Upper halves of all entries stay zero (physical addresses < 4 GiB),
; which .bss zeroing already guarantees.
; -----------------------------------------------------------------------------
setup_page_tables:
    mov eax, pdpt
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pml4], eax

    mov eax, pd
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pdpt], eax

    mov eax, pt
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pd], eax

    xor ecx, ecx                        ; ECX = PT index
.map_page:
    mov eax, ecx
    shl eax, 12                         ; physical address = index * 4096
    or eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pt + ecx * 8], eax             ; low dword; high dword already 0
    inc ecx
    cmp ecx, PAGE_TABLE_ENTRIES
    jb .map_page
    ret

; -----------------------------------------------------------------------------
; enable_long_mode: the architecturally required order is
;   CR4.PAE = 1  ->  CR3 = PML4  ->  EFER.LME = 1  ->  CR0.PG = 1
; After CR0.PG is set, EFER.LMA becomes 1 and the CPU runs in compatibility
; mode until we far jump into a code segment with L = 1.
; -----------------------------------------------------------------------------
enable_long_mode:
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, MSR_EFER
    rdmsr                               ; EDX:EAX = EFER
    or eax, EFER_LME
    wrmsr

    mov eax, cr0
    or eax, CR0_PG | CR0_PE
    mov cr0, eax
    ret

; -----------------------------------------------------------------------------
; boot_error: print "AuraOS boot error: <AL>" in white-on-red and halt.
; Uses only 32-bit code and identity-addressed VGA memory (paging may be off).
; -----------------------------------------------------------------------------
boot_error:
    mov ah, VGA_ERROR_ATTR
    mov ebx, eax                        ; BL = error code char, BH = attribute

    ; Blank the whole first row so leftover bootloader text cannot be
    ; mistaken for part of the message.
    mov edi, VGA_TEXT_BUFFER
    mov ax, (VGA_ERROR_ATTR << 8) | ' '
    mov ecx, VGA_TEXT_WIDTH
    rep stosw

    mov esi, boot_error_msg
    mov edi, VGA_TEXT_BUFFER
.print_msg:
    lodsb                               ; AL = next message character
    test al, al
    jz .print_code
    mov ah, VGA_ERROR_ATTR
    stosw                               ; write (attr << 8) | char
    jmp .print_msg
.print_code:
    mov ax, bx
    stosw
.halt:
    cli
    hlt
    jmp .halt                           ; NMIs/SMIs can wake HLT; loop back

; =============================================================================
; 64-bit long-mode code
; =============================================================================
section .text
bits 64

long_mode_start:
    ; In 64-bit mode DS/ES/SS are ignored for addressing, but they must hold
    ; valid selectors (or null) so that later iretq/sysret checks pass.
    mov ax, GDT_KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Re-establish the stack top. boot_stack_top is 16-byte aligned, so after
    ; "call" pushes the 8-byte return address, kernel_main sees RSP == 8
    ; (mod 16), exactly as the System V AMD64 ABI requires on function entry.
    mov rsp, boot_stack_top
    xor rbp, rbp                        ; terminate frame-pointer chain

    ; System V AMD64 arguments. A 32-bit "mov r32, r32" zero-extends into the
    ; full 64-bit register, which clears any undefined upper bits left over
    ; from the mode switch.
    mov edi, edi                        ; RDI = Multiboot2 magic (uint32_t)
    mov esi, esi                        ; RSI = Multiboot2 info physical addr

    call kernel_main

    ; kernel_main is declared noreturn; if it ever returns, park the CPU.
.hang:
    cli
    hlt
    jmp .hang

; =============================================================================
; Read-only data
; =============================================================================
section .rodata

boot_error_msg:
    db "AuraOS boot error: ", 0

; -----------------------------------------------------------------------------
; 64-bit GDT (Intel SDM Vol. 3A, section 3.4.5).
; In long mode base and limit are ignored for code/data segments; what
; matters is the access byte and the L (long) flag.
;
;   Bit 41  : readable (code) / writable (data)
;   Bit 43  : executable (code segment)
;   Bit 44  : descriptor type, 1 = code/data
;   Bits 45-46 : DPL = 0 (ring 0)
;   Bit 47  : present
;   Bit 53  : L = 1, 64-bit code segment (D bit 54 must then be 0)
; -----------------------------------------------------------------------------
align 16
gdt64:
.null: equ $ - gdt64
    dq 0                                                        ; 0x00 null
.code: equ $ - gdt64
    dq (1 << 41) | (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53) ; 0x08 code
.data: equ $ - gdt64
    dq (1 << 41) | (1 << 44) | (1 << 47)                        ; 0x10 data
.end:
.pointer:
    dw gdt64.end - gdt64 - 1            ; limit = size - 1
    dq gdt64                            ; base (lgdt in 32-bit mode reads the
                                        ; low 4 bytes; 64-bit lgdt reads 8)

; The selectors used in the code above must match the table layout.
times -(gdt64.code != GDT_KERNEL_CODE_SEL) db 0
times -(gdt64.data != GDT_KERNEL_DATA_SEL) db 0

; =============================================================================
; Uninitialised data: page tables and boot stack.
; Every paging structure must be 4 KiB aligned (CR3 and entry bits 12+).
; =============================================================================
section .bss
align PAGE_SIZE
pml4:
    resb PAGE_SIZE
pdpt:
    resb PAGE_SIZE
pd:
    resb PAGE_SIZE
pt:
    resb PAGE_SIZE

align 16
boot_stack_bottom:
    resb STACK_SIZE
boot_stack_top:
