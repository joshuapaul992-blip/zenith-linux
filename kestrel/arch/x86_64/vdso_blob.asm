; arch/x86_64/vdso_blob.asm -- the vDSO image (vdso/vdso.c), embedded in the kernel
;
; The Makefile links vdso/vdso.c into $(BUILD)/vdso/vdso.so and passes its
; path as VDSO_PATH; kernel/vdso.c copies the bytes into the pages every
; process maps.

section .rodata
global vdso_image
global vdso_image_end
align 16
vdso_image:
    incbin VDSO_PATH
vdso_image_end:
