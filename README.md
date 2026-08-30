# Zenith Linux

Zenith Linux is a lightweight, custom independent Linux distribution built completely from scratch without heavy frameworks like Buildroot or Yocto. 

## Features
- **Ultra-lean Footprint:** Monolithic optimized kernel under 10MB.
- **Universal Hardware Support:** Out-of-the-box support for USB 2.0/3.0, NVMe/SATA storage, and generic EFI/VESA framebuffers.
- **Customized Terminal:** Built-in custom console typography/fonts right at boot.
- **Fast Userland:** Powered by a statically-linked modern BusyBox environment.

## How to Test
If you want to run Zenith Linux via QEMU, download the ISO from our Releases page and run:
```bash
qemu-system-x86_64 -cdrom zenith_linux.iso
```
