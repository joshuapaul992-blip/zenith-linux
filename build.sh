#!/usr/bin/env bash
# Zenith Linux build driver.
#
# Usage:
#   ./build.sh            # run all stages: kernel -> busybox -> rootfs -> iso
#   ./build.sh kernel     # run a single stage (kernel|busybox|rootfs|iso)
#   ./build.sh deps       # install host build dependencies
#   ./build.sh run        # boot the built ISO in QEMU
#   ./build.sh clean      # remove build artifacts (keeps the download cache)
#   ./build.sh distclean  # remove everything, including the cache
#
# Override pinned versions:
#   KERNEL_VERSION=6.18 BUSYBOX_VERSION=1.37.0 ./build.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/scripts/common.sh"

stage() { log "stage: $1"; bash "${PROJECT_ROOT}/scripts/$2"; }

run_qemu() {
  require qemu-system-x86_64
  [ -f "$ISO_IMAGE" ] || die "no ISO yet; run ./build.sh first"
  exec qemu-system-x86_64 -m 512 -cdrom "$ISO_IMAGE" "$@"
}

cmd="${1:-all}"
case "$cmd" in
  deps)      bash "${PROJECT_ROOT}/scripts/00-deps.sh" ;;
  kernel)    stage kernel   01-kernel.sh ;;
  busybox)   stage busybox  02-busybox.sh ;;
  rootfs)    stage rootfs   03-rootfs.sh ;;
  iso)       stage iso      04-iso.sh ;;
  run)       shift; run_qemu "$@" ;;
  clean)     rm -rf "$BUILD_DIR/linux-"* "$BUILD_DIR/busybox-"* "$ROOTFS_STAGE" \
                     "$BUILD_DIR/iso" "$KERNEL_IMAGE" "$INITRAMFS" "$OUTPUT_DIR"; ok "cleaned" ;;
  distclean) rm -rf "$BUILD_DIR" "$OUTPUT_DIR"; ok "distcleaned" ;;
  all)
    log "Building Zenith Linux ${ZENITH_VERSION} (kernel ${KERNEL_VERSION}, busybox ${BUSYBOX_VERSION})"
    stage kernel   01-kernel.sh
    stage busybox  02-busybox.sh
    stage rootfs   03-rootfs.sh
    stage iso      04-iso.sh
    ok "done -> ${ISO_IMAGE}"
    ;;
  *) die "unknown command: ${cmd} (try: deps|kernel|busybox|rootfs|iso|run|clean|distclean)" ;;
esac
