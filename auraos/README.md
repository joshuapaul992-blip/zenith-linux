# AuraOS

Minimal x86_64 monolithic kernel booted by GRUB via Multiboot2.

## Phase 1: bootstrapping into 64-bit long mode

| File | Purpose |
|------|---------|
| `multiboot_header.asm` | Multiboot2 header: info request, framebuffer and end tags |
| `boot.asm` | 32-bit `_start`: stack, magic check, CPUID/long-mode check, 4-level identity paging (first 2 MiB), GDT, far jump to 64-bit, call `kernel_main` |
| `linker.ld` | Loads the image at 1 MiB with the Multiboot2 header first; link-time asserts |
| `kernel.c` | Freestanding `kernel_main`: VGA text output and Multiboot2 pointer validation |
| `iso/boot/grub/grub.cfg` | GRUB menu entry (forces text mode; see the comment in it) |

### Host packages (Debian/Ubuntu)

    sudo apt install build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86

On an x86_64 Linux host the system GCC and binutils work with the flags in the
Makefile. An `x86_64-elf` cross toolchain is used automatically if installed
(and is required on non-x86_64 hosts or macOS).

### Build and run

    make          # build/auraos.kernel
    make check    # grub-file --is-x86-multiboot2
    make iso      # build/auraos.iso
    make run      # boot in QEMU

### Boot error codes

If a check in `boot.asm` fails, the first screen row shows
`AuraOS boot error: X` in white on red and the CPU halts:

| Code | Meaning |
|------|---------|
| `0` | Not loaded by a Multiboot2 bootloader (bad magic in EAX) |
| `1` | CPUID instruction not supported |
| `2` | CPU does not support 64-bit long mode |
