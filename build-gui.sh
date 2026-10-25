#!/usr/bin/env bash
# Zenith Linux GUI-tier build driver -> output/zenith-gui.iso
#
# A separate, larger live ISO that recreates the Damn Small Linux 4.4.10
# desktop (Xorg + JWM/Fluxbox + xfe + conky) on the modern kernel. The tiny
# zenith-core.iso (build.sh) is unaffected.
#
# Usage:
#   sudo ./build-gui.sh           # full build (rootfs stage needs root)
#   ./build-gui.sh deps           # install host build dependencies
#   sudo ./build-gui.sh rootfs    # a single stage
#   ./build-gui.sh run            # boot the GUI ISO in QEMU
#   ./build-gui.sh clean
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build the GUI tier in its own dir with the richer kernel fragment stack.
export BUILD_DIR="${PROJECT_ROOT}/build/gui"
export KERNEL_FRAGMENTS="${PROJECT_ROOT}/config/kernel.fragment ${PROJECT_ROOT}/config/kernel-gui.fragment"

source "${PROJECT_ROOT}/scripts/common.sh"

stage() { log "stage: $1"; bash "${PROJECT_ROOT}/$2"; }

run_qemu() {
  require qemu-system-x86_64
  [ -f "$GUI_ISO" ] || die "no GUI ISO yet; run ./build-gui.sh first"
  exec qemu-system-x86_64 -m 1024 -cdrom "$GUI_ISO" -vga std "$@"
}

cmd="${1:-all}"
case "$cmd" in
  deps)    bash "${PROJECT_ROOT}/scripts/00-deps.sh" ;;
  kernel)  stage kernel        scripts/01-kernel.sh ;;
  busybox) stage busybox       scripts/02-busybox.sh ;;
  rootfs)  stage gui-rootfs    scripts/gui/rootfs.sh ;;
  squashfs)stage squashfs      scripts/gui/squashfs.sh ;;
  initramfs)stage live-initramfs scripts/gui/initramfs.sh ;;
  iso)     stage gui-iso       scripts/gui/iso.sh ;;
  run)     shift; run_qemu "$@" ;;
  clean)   rm -rf "$BUILD_DIR" "$GUI_ISO"; ok "cleaned GUI build" ;;
  all)
    log "Building Zenith Linux GUI ISO (kernel ${KERNEL_VERSION}, Ubuntu ${UBUNTU_VERSION} + DSL ${DSL_VERSION})"
    stage kernel         scripts/01-kernel.sh
    stage busybox        scripts/02-busybox.sh
    stage gui-rootfs     scripts/gui/rootfs.sh
    stage squashfs       scripts/gui/squashfs.sh
    stage live-initramfs scripts/gui/initramfs.sh
    stage gui-iso        scripts/gui/iso.sh
    ok "done -> ${GUI_ISO}"
    ;;
  *) die "unknown command: ${cmd} (try: deps|kernel|busybox|rootfs|squashfs|initramfs|iso|run|clean)" ;;
esac
