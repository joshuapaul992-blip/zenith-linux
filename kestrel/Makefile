# =============================================================================
#  Kestrel -- freestanding x86_64 kernel with a BIOS-style boot manager UI
#
#  make              build build/kernel.elf
#  make iso          bootable hybrid ISO (BIOS + UEFI) via grub-mkrescue
#  make floppy       1.44 MB BIOS floppy image (GRUB core.img + memdisk)
#  make run          boot the ISO in QEMU (legacy BIOS, VBE)
#  make run-uefi     boot the ISO in QEMU with OVMF firmware (UEFI, GOP)
#  make run-floppy   boot the floppy image in QEMU
#  make usbimg       64 MiB USB stick image with GRUB + a Kestrel boot volume
#  make run-usb      boot the stick from a QEMU xHCI port (kestrel.root=usb)
#  make xlibre       build the X server for Kestrel (see ports/xlibre/build.sh)
#  make icewm        build the IceWM window manager (see ports/icewm/build.sh)
#  make busybox      build BusyBox: /bin/sh and tools (see ports/busybox/build.sh)
#  make xterm        build xterm (see ports/xterm/build.sh)
#  make debug        QEMU halted with a gdb stub on :1234
#  make check        verify the Multiboot2 header with grub-file
#  make fonts        regenerate drivers/font_*.c from the BDF sources
#  make clean
#
#  Requirements: gcc (or x86_64-elf-gcc), GNU ld, nasm, GNU make; for images:
#  grub-mkrescue + xorriso + mtools (ISO), grub-mkimage (floppy); qemu to run.
#  Debian/Ubuntu: apt install build-essential nasm grub-pc-bin grub-efi-amd64-bin
#                             grub-common xorriso mtools qemu-system-x86 ovmf
# =============================================================================

# ---- toolchain (a host x86_64 gcc works because everything is freestanding)
ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
  CROSS ?= x86_64-elf-
endif
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
NASM    ?= nasm
PYTHON  ?= python3
QEMU    ?= qemu-system-x86_64
HOSTCC  ?= cc

GRUB_MKRESCUE ?= $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)
GRUB_MKIMAGE  ?= $(shell command -v grub-mkimage  2>/dev/null || command -v grub2-mkimage  2>/dev/null)
GRUB_FILE     ?= $(shell command -v grub-file     2>/dev/null || command -v grub2-file     2>/dev/null)
GRUB_LIB      ?= $(firstword $(wildcard /usr/lib/grub /usr/lib/grub2 /usr/local/lib/grub /usr/share/grub2))
OVMF          ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
                   /usr/share/ovmf/OVMF.fd /usr/share/edk2/x64/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd \
                   /usr/share/qemu/edk2-x86_64-code.fd))

# ---- flags
CFLAGS := -std=gnu11 -O2 -g -pipe \
          -ffreestanding -fno-builtin-printf -nostdlib \
          -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
          -fno-tree-loop-distribute-patterns \
          -m64 -march=x86-64 -mcmodel=small -mno-red-zone \
          -mno-mmx -mno-sse -mno-sse2 -mno-80387 -mgeneral-regs-only \
          -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -Idrivers/pci -Idrivers/ahci -Idrivers/nvme -Idrivers/usb -Idrivers/video -Idrivers/gpu/nvidia -MMD -MP
# -mcmodel=small: the kernel is linked/identity-mapped at 2 MiB (below 2 GiB).
# A future higher-half move (0xFFFFFFFF80000000) would switch to -mcmodel=kernel.

NASMFLAGS := -f elf64 -g -F dwarf -w+all
LDFLAGS   := -nostdlib -static -no-pie -T linker.ld -z max-page-size=0x1000 \
             -z noexecstack --no-warn-rwx-segments

# ---- sources
BUILD := build
ASM_SRC := boot/multiboot2_header.asm boot/boot.asm \
           arch/x86_64/isr.asm arch/x86_64/switch.asm arch/x86_64/syscall_entry.asm
C_SRC   := kernel/kernel.c kernel/klog.c kernel/syscall.c \
           arch/x86_64/gdt.c arch/x86_64/idt.c arch/x86_64/pic.c arch/x86_64/pit.c \
           drivers/serial.c drivers/fb.c drivers/console.c drivers/keyboard.c drivers/kinput.c \
           drivers/font_8x16.c drivers/font_12x24.c drivers/pci/pci.c \
           drivers/ahci/ahci.c drivers/nvme/nvme.c kernel/storage.c drivers/usb/xhci.c kernel/usbhost.c \
           drivers/usb/usb_legacy.c drivers/usb/usb_msc.c kernel/time.c \
           kernel/block.c kernel/bootvol.c kernel/recovery.c fs/tarfs.c fs/pipe.c fs/pty.c fs/unixsock.c lib/crc32.c \
           drivers/kestrel_font.c drivers/video/edid.c drivers/video/bochs_dispi.c \
           kernel/display.c kernel/tty.c kernel/report.c \
           drivers/gpu/nvidia/nvbios.c drivers/gpu/nvidia/nvbios_disp.c drivers/gpu/nvidia/nvfmt.c \
           drivers/gpu/nvidia/nvinit.c drivers/gpu/nvidia/nvdisp.c drivers/gpu/nvidia/nvidia.c kernel/gpu.c kernel/kush.c \
           kernel/coreutils/cu_lib.c kernel/coreutils/table.c kernel/coreutils/nav.c \
           kernel/coreutils/ls.c kernel/coreutils/fileops.c kernel/coreutils/copy.c \
           kernel/coreutils/text.c kernel/coreutils/perms.c kernel/coreutils/sysinfo.c \
           kernel/coreutils/neofetch.c \
           mm/pmm.c mm/heap.c mm/vmm.c mm/uvm.c \
           proc/task.c proc/exec.c proc/process.c \
           fs/vfs.c fs/devfs.c fs/procfs.c fs/sysfs.c \
           ui/bootmgr.c \
           lib/string.c lib/printf.c

OBJS   := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC)) $(patsubst %.c,$(BUILD)/%.o,$(C_SRC))
KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/kestrel.iso
FLOPPY := $(BUILD)/kestrel-floppy.img

QEMU_FLAGS ?= -m 256M -vga std -serial stdio -no-reboot

# Kernel command line baked into the GRUB configs, e.g.
#   make iso KERNEL_CMDLINE="ahci.selftest=rw ahci.verify=5"
KERNEL_CMDLINE ?=

.PHONY: FORCE xlibre icewm busybox xterm all iso floppy usbimg run-usb run run-uefi run-floppy run-pci debug check test test-pci test-nvbios test-nvidia fonts clean distclean

all: $(KERNEL)

# ---- compile & link
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM    $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "  kernel: $$(stat -c %s $@) bytes"

# ---- bootable ISO (BIOS El Torito + UEFI, whatever GRUB platforms are installed)
$(BUILD)/cmdline.stamp: FORCE
	@mkdir -p $(BUILD)
	@echo '$(KERNEL_CMDLINE)' | cmp -s - $@ || echo '$(KERNEL_CMDLINE)' > $@

$(ISO): $(KERNEL) boot/grub/grub.cfg $(BUILD)/cmdline.stamp
	@test -n "$(GRUB_MKRESCUE)" || { echo "error: grub-mkrescue not found"; exit 1; }
	@rm -rf $(BUILD)/isodir
	@mkdir -p $(BUILD)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD)/isodir/boot/kernel.elf
	@sed 's|@KERNEL_CMDLINE@|$(KERNEL_CMDLINE)|' boot/grub/grub.cfg > $(BUILD)/isodir/boot/grub/grub.cfg
	@echo "  ISO     $@"
	@$(GRUB_MKRESCUE) -o $@ $(BUILD)/isodir 2> $(BUILD)/grub-mkrescue.log || \
	    { cat $(BUILD)/grub-mkrescue.log; exit 1; }

iso: $(ISO)

# Debug info stays in build/kernel.elf; images that embed the kernel in a
# GRUB core.img (limited to 480 KiB, compressed) get a stripped copy.
STRIPPED := $(BUILD)/kernel-stripped.elf
$(STRIPPED): $(KERNEL)
	@$(OBJCOPY) --strip-debug $< $@

# ---- 1.44 MB floppy: boot.img + core.img with the kernel inside a memdisk
$(FLOPPY): $(STRIPPED) boot/grub/grub-floppy.cfg $(BUILD)/cmdline.stamp
	@test -n "$(GRUB_MKIMAGE)" || { echo "error: grub-mkimage not found"; exit 1; }
	@test -f "$(GRUB_LIB)/i386-pc/boot.img" || { echo "error: GRUB i386-pc files not found in GRUB_LIB=$(GRUB_LIB)"; exit 1; }
	@rm -rf $(BUILD)/floppydir
	@mkdir -p $(BUILD)/floppydir/boot/grub
	@cp $(STRIPPED) $(BUILD)/floppydir/boot/kernel.elf
	@sed 's|@KERNEL_CMDLINE@|$(KERNEL_CMDLINE)|' boot/grub/grub-floppy.cfg > $(BUILD)/floppydir/boot/grub/grub.cfg
	@tar --format=ustar -C $(BUILD)/floppydir -cf $(BUILD)/memdisk.tar boot
	@echo "  GRUB    core.img (memdisk)"
	@$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_LIB)/i386-pc -o $(BUILD)/core.img \
	    -p '(memdisk)/boot/grub' -m $(BUILD)/memdisk.tar \
	    biosdisk memdisk tar normal configfile multiboot2 vbe video video_fb
	@echo "  FLOPPY  $@"
	@cat $(GRUB_LIB)/i386-pc/boot.img $(BUILD)/core.img > $@
	@test $$(stat -c %s $@) -le 1474560 || { echo "error: image exceeds 1.44 MB"; exit 1; }
	@truncate -s 1474560 $@

floppy: $(FLOPPY)

# ---- USB stick: GRUB boot.img + core.img (kernel in memdisk, kestrel.root=usb)
#      and an MBR partition of type 0x4b holding the Kestrel boot volume
#      (header + ustar of rootfs/); sector 0 also carries the option-B marker.
#      For UEFI machines an EFI System Partition holds EFI/BOOT/BOOTX64.EFI
#      (GRUB x86_64-efi with the same memdisk), when GRUB's EFI files and
#      mtools are installed. A FAT volume "KESTREL RPT" receives REPORT.TXT.
# ---- ring-3 programs (user/), installed into the boot volume's /bin -------
# Freestanding static-PIE for now; musl-linked programs come next.
USER_CC     ?= $(HOSTCC)
USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fPIE -fno-stack-protector \
               -fno-asynchronous-unwind-tables -nostdlib -static-pie -Wl,--no-dynamic-linker
# Programs linked against musl (Kestrel uses the Linux syscall ABI, so the
# stock x86_64 musl works unchanged); static-PIE, self-relocating via rcrt1.
MUSL_CC     ?= musl-gcc
MUSL_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -fPIE -static-pie
MUSL_PROGS  := libctest socktest proctest ptytest
USER_PROGS  := $(BUILD)/user/hello $(if $(shell command -v $(MUSL_CC)),$(addprefix $(BUILD)/user/,$(MUSL_PROGS)))

$(addprefix $(BUILD)/user/,$(MUSL_PROGS)): $(BUILD)/user/%: user/%.c
	@mkdir -p $(dir $@)
	@echo "  MUSLCC  $<"
	@$(MUSL_CC) $(MUSL_CFLAGS) $< -o $@

$(BUILD)/user/hello: $(BUILD)/user/%: user/%.c
	@mkdir -p $(dir $@)
	@echo "  USERCC  $<"
	@$(USER_CC) $(USER_CFLAGS) $< -o $@

# ---- X11 (ports/xlibre): optional, built by `make xlibre` ------------------
#   make xlibre XLIBRE_SRC=<xserver checkout> XLIBRE_DEPS=<library tarballs>
# installs into $(XLIBRE_ROOT); the rootfs staging below picks it up whenever
# it exists (Xkestrel, xdemo, the precompiled keymap).
XLIBRE_SRC  ?=
XLIBRE_DEPS ?=
XLIBRE_ROOT := $(BUILD)/xlibre-root
xlibre:
	@test -n "$(XLIBRE_SRC)" -a -n "$(XLIBRE_DEPS)" || \
	    { echo "usage: make xlibre XLIBRE_SRC=<xserver checkout> XLIBRE_DEPS=<tarball dir>"; exit 1; }
	@ports/xlibre/build.sh $(XLIBRE_SRC) $(XLIBRE_DEPS) $(BUILD)

# IceWM (ports/icewm), after `make xlibre`:
#   make icewm ICEWM_SRC=<unpacked icewm 3.x release>
ICEWM_SRC  ?=
ICEWM_ROOT := $(BUILD)/icewm-root
icewm:
	@test -n "$(ICEWM_SRC)" || { echo "usage: make icewm ICEWM_SRC=<unpacked icewm release>"; exit 1; }
	@ports/icewm/build.sh $(ICEWM_SRC) $(BUILD)

# BusyBox (ports/busybox): /bin/sh and the command-line tools
#   make busybox BUSYBOX_SRC=<unpacked busybox release>
BUSYBOX_SRC  ?=
BUSYBOX_ROOT := $(BUILD)/busybox-root
busybox:
	@test -n "$(BUSYBOX_SRC)" || { echo "usage: make busybox BUSYBOX_SRC=<unpacked busybox release>"; exit 1; }
	@ports/busybox/build.sh $(BUSYBOX_SRC) $(BUILD)

# xterm (ports/xterm), after the X libraries:
#   make xterm XTERM_SRC=<unpacked xterm release>
XTERM_SRC  ?=
XTERM_ROOT := $(BUILD)/xterm-root
xterm:
	@test -n "$(XTERM_SRC)" || { echo "usage: make xterm XTERM_SRC=<unpacked xterm release>"; exit 1; }
	@ports/xterm/build.sh $(XTERM_SRC) $(BUILD)

ROOTFS_STAGE := $(BUILD)/rootfs
$(ROOTFS_STAGE).stamp: $(shell find rootfs -type f) $(USER_PROGS) user/shtest.sh $(wildcard $(XLIBRE_ROOT)/.stamp) $(wildcard $(ICEWM_ROOT)/.stamp) $(wildcard $(BUSYBOX_ROOT)/.stamp) $(wildcard $(XTERM_ROOT)/.stamp)
	@rm -rf $(ROOTFS_STAGE)
	@mkdir -p $(ROOTFS_STAGE)/bin
	@cp -r rootfs/. $(ROOTFS_STAGE)/
	@cp $(USER_PROGS) $(ROOTFS_STAGE)/bin/
	@install -m 755 user/shtest.sh $(ROOTFS_STAGE)/bin/shtest.sh
	@if [ -f $(XLIBRE_ROOT)/.stamp ]; then cp -r $(XLIBRE_ROOT)/bin $(XLIBRE_ROOT)/share $(ROOTFS_STAGE)/; fi
	@if [ -f $(ICEWM_ROOT)/.stamp ]; then cp -r $(ICEWM_ROOT)/bin $(ICEWM_ROOT)/share $(ICEWM_ROOT)/etc $(ROOTFS_STAGE)/; fi
	@if [ -f $(BUSYBOX_ROOT)/.stamp ]; then cp -a $(BUSYBOX_ROOT)/bin $(ROOTFS_STAGE)/; fi   # -a: keep the hard links
	@if [ -f $(XTERM_ROOT)/.stamp ]; then cp -r $(XTERM_ROOT)/bin $(XTERM_ROOT)/share $(ROOTFS_STAGE)/; fi
	@touch $@

USBIMG := $(BUILD)/kestrel-usb.img
USBIMG_FLAGS ?=
$(USBIMG): $(STRIPPED) boot/grub/grub-usb.cfg tools/mkusbimg.py $(ROOTFS_STAGE).stamp $(BUILD)/cmdline.stamp
	@test -n "$(GRUB_MKIMAGE)" || { echo "error: grub-mkimage not found"; exit 1; }
	@rm -rf $(BUILD)/usbdir
	@mkdir -p $(BUILD)/usbdir/boot/grub
	@cp $(STRIPPED) $(BUILD)/usbdir/boot/kernel.elf
	@sed 's|@KERNEL_CMDLINE@|$(KERNEL_CMDLINE)|' boot/grub/grub-usb.cfg > $(BUILD)/usbdir/boot/grub/grub.cfg
	@tar --format=ustar -C $(BUILD)/usbdir -cf $(BUILD)/usb-memdisk.tar boot
	@echo "  GRUB    core.img (USB, memdisk)"
	@$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_LIB)/i386-pc -o $(BUILD)/usb-core.img \
	    -p '(memdisk)/boot/grub' -m $(BUILD)/usb-memdisk.tar \
	    biosdisk memdisk tar normal configfile multiboot2 vbe video video_fb test
	@rm -f $(BUILD)/usb-esp.img
	@if [ -d "$(GRUB_LIB)/x86_64-efi" ] && command -v mformat >/dev/null; then \
	    echo "  GRUB    BOOTX64.EFI (USB, memdisk)"; \
	    $(GRUB_MKIMAGE) -O x86_64-efi -d $(GRUB_LIB)/x86_64-efi -o $(BUILD)/usb-bootx64.efi \
	        -p '(memdisk)/boot/grub' -m $(BUILD)/usb-memdisk.tar \
	        memdisk tar normal configfile multiboot2 efi_gop efi_uga video video_fb test && \
	    truncate -s 8M $(BUILD)/usb-esp.img && \
	    MTOOLS_SKIP_CHECK=1 mformat -i $(BUILD)/usb-esp.img -T 16384 -h 64 -s 32 -v KESTREL_EFI :: && \
	    MTOOLS_SKIP_CHECK=1 mmd -i $(BUILD)/usb-esp.img ::/EFI ::/EFI/BOOT && \
	    MTOOLS_SKIP_CHECK=1 mcopy -i $(BUILD)/usb-esp.img $(BUILD)/usb-bootx64.efi ::/EFI/BOOT/BOOTX64.EFI; \
	else echo "  (no GRUB x86_64-efi files or mtools: BIOS-only stick)"; fi
	@echo "  USBIMG  $@"
	@$(PYTHON) tools/mkusbimg.py $@ --rootfs $(ROOTFS_STAGE) --boot-img $(GRUB_LIB)/i386-pc/boot.img \
	    --core-img $(BUILD)/usb-core.img $$(test -f $(BUILD)/usb-esp.img && echo --esp-img $(BUILD)/usb-esp.img) \
	    $(USBIMG_FLAGS)

usbimg: $(USBIMG)

# Boot from the stick on xHCI (SeaBIOS boots USB mass storage; bootindex=0)
run-usb: $(USBIMG)
	$(QEMU) $(QEMU_FLAGS) -machine q35 -device qemu-xhci,id=xhci \
	    -drive if=none,id=stick,format=raw,file=$(USBIMG) \
	    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
	    -device usb-kbd,bus=xhci.0

# ---- run / debug
run: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO)

run-uefi: $(ISO)
	@test -n "$(OVMF)" || { echo "error: OVMF firmware not found (set OVMF=/path/to/OVMF.fd)"; exit 1; }
	$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) -cdrom $(ISO)

run-floppy: $(FLOPPY)
	$(QEMU) $(QEMU_FLAGS) -drive if=floppy,format=raw,file=$(FLOPPY) -boot a

debug: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -s -S &
	@echo "gdb $(KERNEL) -ex 'target remote :1234'"

# Boot on a q35 machine with PCIe storage/USB to exercise the PCI enumerator:
# AHCI is built into q35 (00:1f.2), NVMe sits behind a root port, and the
# xHCI controller behind a root port + PCIe-to-PCI bridge (two levels deep).
# A 200 GiB sparse SATA disk with a test MBR hangs off AHCI port 0.
PCI_TEST_DEVICES := -machine q35 \
    -drive if=none,id=sata0,file=$(BUILD)/sata.img,format=raw \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -device pcie-root-port,id=rp1,chassis=1,slot=1 \
    -drive if=none,id=nvme0,file=$(BUILD)/nvme.img,format=raw \
    -device nvme,serial=KESTREL0001,drive=nvme0,bus=rp1 \
    -device pcie-root-port,id=rp2,chassis=2,slot=2 \
    -device pcie-pci-bridge,id=pb1,bus=rp2 \
    -device qemu-xhci,id=xhci,bus=pb1,addr=1 \
    -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0

$(BUILD)/nvme.img:
	@mkdir -p $(BUILD) && truncate -s 64M $@

$(BUILD)/sata.img:
	@mkdir -p $(BUILD) && $(PYTHON) tools/mkdisk.py $@ 200G

run-pci: $(ISO) $(BUILD)/nvme.img $(BUILD)/sata.img
	$(QEMU) $(QEMU_FLAGS) $(PCI_TEST_DEVICES) -cdrom $(ISO) -boot d

# Host-side unit test of the enumerator against a simulated config space
test-pci:
	@mkdir -p $(BUILD)
	@$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	    -DPCI_PORT_IO_HOOKS -DPCI_NO_MEM_FALLBACK -Idrivers/pci \
	    drivers/pci/test/pci_sim_test.c drivers/pci/pci.c -o $(BUILD)/pci_sim_test
	@$(BUILD)/pci_sim_test | tail -1
	@$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	    -DPCI_PORT_IO_HOOKS -DPCI_NO_MEM_FALLBACK -DPCI_MAX_TRACKED=4 -Idrivers/pci \
	    drivers/pci/test/pci_sim_test.c drivers/pci/pci.c -o $(BUILD)/pci_sim_test_small
	@$(BUILD)/pci_sim_test_small | tail -1

# Host-side unit test of the NVIDIA VBIOS parser; NVBIOS_ROM=card.rom also
# parses and prints a real ROM dump (e.g. VBIOS.ROM from the report stick)
test-nvbios:
	@mkdir -p $(BUILD)
	@$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Idrivers/gpu/nvidia \
	    drivers/gpu/nvidia/test/nvbios_test.c drivers/gpu/nvidia/nvbios.c -o $(BUILD)/nvbios_test
	@$(BUILD)/nvbios_test $(NVBIOS_ROM)

# Host-side test of the NVIDIA probe against a simulated GP106 BAR0
test-nvidia:
	@mkdir -p $(BUILD)
	@$(HOSTCC) -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -DNV_MMIO_HOOKS \
	    -Idrivers/gpu/nvidia -Idrivers/gpu/nvidia/test drivers/gpu/nvidia/test/nvidia_sim_test.c \
	    drivers/gpu/nvidia/nvidia.c drivers/gpu/nvidia/nvbios.c drivers/gpu/nvidia/nvfmt.c -o $(BUILD)/nvidia_sim_test
	@$(BUILD)/nvidia_sim_test > $(BUILD)/nvidia_sim_test.log; rc=$$?; tail -1 $(BUILD)/nvidia_sim_test.log; exit $$rc

test: test-pci test-nvbios test-nvidia

check: $(KERNEL)
	@test -n "$(GRUB_FILE)" || { echo "error: grub-file not found"; exit 1; }
	@$(GRUB_FILE) --is-x86-multiboot2 $(KERNEL) && echo "  $(KERNEL): valid Multiboot2 header"

# ---- fonts (Spleen, BSD-2-Clause, third_party/spleen)
fonts:
	$(PYTHON) tools/sheet2font.py third_party/kestrel_font/kestrel_font_sheet.jpg drivers/kestrel_font.c
	$(PYTHON) tools/bdf2c.py third_party/spleen/spleen-8x16.bdf  font_8x16  > drivers/font_8x16.c
	$(PYTHON) tools/bdf2c.py third_party/spleen/spleen-12x24.bdf font_12x24 > drivers/font_12x24.c

clean:
	rm -rf $(BUILD)

distclean: clean

-include $(OBJS:.o=.d)
