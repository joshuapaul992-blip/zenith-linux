# Zenith Linux

Zenith Linux is a lightweight, custom independent Linux distribution built
completely from scratch without heavy frameworks like Buildroot or Yocto.

Think **Damn Small Linux 4.4.10 — but on a modern Linux 6.18 kernel.** It keeps
the DSL spirit (tiny footprint, RAM-resident, boots almost anywhere) while the
current kernel brings hardware support DSL's ancient 2.4/2.6 kernels never had:
xHCI/USB 3.0, NVMe, AHCI SATA, and EFI/simpledrm framebuffers.

## Features
- **Ultra-lean Footprint:** Monolithic, module-free kernel built from
  `tinyconfig` + a curated option set.
- **Universal Hardware Support:** Out-of-the-box USB 2.0/3.0, NVMe/SATA storage,
  and generic EFI/VESA framebuffers.
- **Fast Userland:** Powered by a statically-linked modern BusyBox environment.

## Versions
| Component | Version | Override |
|-----------|---------|----------|
| Linux kernel | `6.18` | `KERNEL_VERSION=` |
| BusyBox | `1.37.0` | `BUSYBOX_VERSION=` |

## Building

The build is four plain shell stages, driven by `build.sh` (or `make`):
kernel → busybox → rootfs/initramfs → bootable ISO.

```bash
./build.sh deps     # install host build dependencies (apt/dnf/pacman)
./build.sh          # build everything -> output/zenith_linux.iso
```

Individual stages, for iterating:

```bash
./build.sh kernel    # fetch + build the 6.18 bzImage
./build.sh busybox   # fetch + build static busybox
./build.sh rootfs    # assemble rootfs, pack initramfs.cpio.gz
./build.sh iso       # produce the bootable hybrid ISO
```

Downloads are cached under `build/cache/`, so re-runs are fast. `make clean`
removes build artifacts; `make distclean` also drops the cache.

> Building the kernel from source needs a few GB of disk and several minutes of
> CPU. If you'd rather not build locally, every push runs the
> [`Build Zenith Linux ISO`](.github/workflows/build.yml) GitHub Actions
> workflow, which uploads `zenith_linux.iso` as an artifact (and attaches it to
> releases tagged `v*`).

## How to Test
Boot the ISO in QEMU:
```bash
qemu-system-x86_64 -m 512 -cdrom output/zenith_linux.iso
# or, headless over serial:
qemu-system-x86_64 -m 512 -cdrom output/zenith_linux.iso -nographic
```
Or just: `./build.sh run`

You boot straight to a root shell — no login. Press `Ctrl-Alt-Del` (or run
`reboot`) to shut down cleanly.

## Layout
```
build.sh                 # build driver (also: make)
config/
  kernel.fragment        # options merged onto `make tinyconfig`
  isolinux.cfg           # bootloader menu
scripts/
  00-deps.sh             # host dependency installer
  01-kernel.sh           # kernel stage
  02-busybox.sh          # busybox stage
  03-rootfs.sh           # rootfs + initramfs stage
  04-iso.sh              # ISO stage
  common.sh              # shared config + helpers
rootfs/                  # tracked root filesystem skeleton (init, /etc, ...)
```
