/* vdso/vdso.lds -- layout of the vDSO image (after Linux's vdso-layout.lds)
 *
 * One read-and-execute PT_LOAD from address 0 holding the ELF header, the
 * dynamic symbol tables (both hash styles: musl reads DT_HASH, glibc
 * DT_GNU_HASH), the dynamic section and the code. `vvar` is the data page
 * the kernel maps right before it. */
SECTIONS
{
    vvar = . - 4096;
    . = SIZEOF_HEADERS;

    .hash           : { *(.hash) }                  :text
    .gnu.hash       : { *(.gnu.hash) }
    .dynsym         : { *(.dynsym) }
    .dynstr         : { *(.dynstr) }
    .gnu.version    : { *(.gnu.version) }
    .gnu.version_d  : { *(.gnu.version_d) }
    .gnu.version_r  : { *(.gnu.version_r) }
    .dynamic        : { *(.dynamic) }               :text :dynamic
    .rodata         : { *(.rodata*) }               :text
    .note           : { *(.note.*) }                :text :note
    .text           : { *(.text*) }                 :text =0x90909090

    /DISCARD/ : { *(.data*) *(.bss*) *(.got*) *(.plt*) *(.comment) *(.note.GNU-stack) *(.eh_frame*) }
}

PHDRS
{
    text            PT_LOAD FLAGS(5) FILEHDR PHDRS;     /* R+X */
    dynamic         PT_DYNAMIC FLAGS(4);
    note            PT_NOTE FLAGS(4);
}

VERSION
{
    LINUX_2.6 {
    global:
        clock_gettime; __vdso_clock_gettime;
        gettimeofday;  __vdso_gettimeofday;
        time;          __vdso_time;
        clock_getres;  __vdso_clock_getres;
        getcpu;        __vdso_getcpu;
    local: *;
    };
}
