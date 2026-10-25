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

## Editions
Zenith ships as tiered ISOs that share the same from-scratch kernel:

| ISO | Build | What it is | Size |
|-----|-------|-----------|------|
| `zenith_linux.iso` | `./build.sh` | Tiny RAM-resident BusyBox core — "damn small" | ~4 MB |
| `zenith-gui.iso` | `sudo ./build-gui.sh` | Live **DSL 4.4.10-style desktop**: Xorg + JWM/Fluxbox + xfe + conky | ~110 MB |

## Versions
| Component | Version | Override |
|-----------|---------|----------|
| Linux kernel | `6.18` | `KERNEL_VERSION=` |
| BusyBox | `1.37.0` | `BUSYBOX_VERSION=` |
| Alpine (GUI userland) | `3.24.3` | `ALPINE_VERSION=` |

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

## The GUI edition (`zenith-gui.iso`)
A faithful recreation of the **Damn Small Linux 4.4.10** desktop on the modern
kernel — the classic JWM bottom panel, a torsmo-style monitor floating
top-right, the blue DSL backdrop — rebuilt from current components:

- **Xorg** on the generic framebuffer/VESA path (just like DSL's own Xvesa), so
  it boots without GPU-specific drivers.
- **JWM** (default) and **Fluxbox**, switchable at runtime from the menu.
- **xfe** file manager, **xterm**, and a **conky** system monitor.
- A small **Alpine (musl)** userland, delivered as a compressed SquashFS that is
  overlaid with a tmpfs at boot (live-CD style — the system runs from RAM).

It is a separate, larger ISO; the tiny core edition is unchanged. No browser or
game clients are included yet.

```bash
sudo ./build-gui.sh          # the rootfs stage needs root (chroot/apk)
qemu-system-x86_64 -m 1024 -cdrom output/zenith-gui.iso -vga std
```
You boot straight to the desktop on tty1 (rescue shells on tty2 and ttyS0).

## How to Test
Boot the core ISO in QEMU:
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
build.sh                 # core build driver (also: make)
build-gui.sh             # GUI edition build driver
config/
  kernel.fragment        # options merged onto `make tinyconfig`
  kernel-gui.fragment    # extra options for the GUI edition (X, DRM, squashfs)
  isolinux.cfg           # core bootloader menu
  isolinux-gui.cfg       # GUI bootloader menu
scripts/
  00-deps.sh             # host dependency installer
  01-kernel.sh           # kernel stage (shared; KERNEL_FRAGMENTS selects config)
  02-busybox.sh          # busybox stage (shared)
  03-rootfs.sh           # core rootfs + initramfs stage
  04-iso.sh              # core ISO stage
  common.sh              # shared config + helpers
  gui/                   # GUI stages: rootfs (Alpine), squashfs, initramfs, iso
rootfs/                  # core root filesystem skeleton (init, /etc, ...)
rootfs-live/             # live initramfs PID 1 (squashfs + overlay + switch_root)
gui-overlay/             # DSL-style desktop theme laid over the Alpine rootfs
```
