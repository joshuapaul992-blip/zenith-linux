# Thin convenience wrapper around build.sh.
.PHONY: all deps kernel busybox rootfs iso run clean distclean

all:        ; @./build.sh all
deps:       ; @./build.sh deps
kernel:     ; @./build.sh kernel
busybox:    ; @./build.sh busybox
rootfs:     ; @./build.sh rootfs
iso:        ; @./build.sh iso
run:        ; @./build.sh run
clean:      ; @./build.sh clean
distclean:  ; @./build.sh distclean
