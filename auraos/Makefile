# =============================================================================
# AuraOS - Makefile (Phase 1)
#
#   make          build build/auraos.kernel (ELF64, Multiboot2)
#   make iso      build build/auraos.iso (GRUB BIOS bootable)
#   make run      boot the ISO in QEMU
#   make check    verify the Multiboot2 header with grub-file
#   make clean
#
# A dedicated x86_64-elf cross toolchain is used when installed; otherwise the
# host's x86_64 GCC/binutils are used with flags that make them freestanding.
# =============================================================================

ifneq ($(shell command -v x86_64-elf-gcc 2>/dev/null),)
    CC := x86_64-elf-gcc
    LD := x86_64-elf-ld
else
    CC := gcc
    LD := ld
endif
AS         := nasm
GRUB_MKRESCUE ?= grub-mkrescue
QEMU       ?= qemu-system-x86_64

BUILD := build
KERNEL := $(BUILD)/auraos.kernel
ISO    := $(BUILD)/auraos.iso

ASFLAGS := -f elf64 -w+all -w+error

# Freestanding, kernel-safe code generation:
#   -ffreestanding -fno-builtin  no hosted libc assumptions
#   -fno-stack-protector         no __stack_chk_fail / TLS canary (no libc)
#   -fno-pic -fno-pie            absolute addressing, linked at 1 MiB
#   -mcmodel=kernel would need a -2 GiB link address; "small" fits 1 MiB
#   -mno-red-zone                interrupts must not clobber below-RSP data
#   -mgeneral-regs-only          no SSE/x87/MMX: FPU state is not saved yet,
#                                and __attribute__((interrupt)) requires it
CFLAGS := -std=c11 -O2 -g \
          -ffreestanding -fno-builtin -nostdlib -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -fno-stack-protector -fno-stack-check \
          -fno-pic -fno-pie -mcmodel=small \
          -fno-omit-frame-pointer \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -m64 -march=x86-64 -mno-red-zone -mgeneral-regs-only \
          -Wall -Wextra -Werror -Wpedantic -Wshadow -Wcast-align \
          -Wstrict-prototypes -Wmissing-prototypes -Wconversion

LDFLAGS := -m elf_x86_64 -T linker.ld -nostdlib -static -no-pie \
           -z max-page-size=0x1000 -z noexecstack --build-id=none

# Order is irrelevant for placement (the linker script decides), but keeping
# the header first makes the map file easy to read.
OBJS := $(BUILD)/multiboot_header.o $(BUILD)/boot.o $(BUILD)/kernel.o

.PHONY: all iso run check clean

all: $(KERNEL)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.asm | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -Map=$(BUILD)/auraos.map -o $@ $(OBJS)

check: $(KERNEL)
	grub-file --is-x86-multiboot2 $(KERNEL) && echo "Multiboot2 header OK"

iso: $(ISO)

$(ISO): $(KERNEL) iso/boot/grub/grub.cfg
	rm -rf $(BUILD)/isodir
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/auraos.kernel
	cp iso/boot/grub/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/isodir

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 128M -no-reboot -no-shutdown

clean:
	rm -rf $(BUILD)
