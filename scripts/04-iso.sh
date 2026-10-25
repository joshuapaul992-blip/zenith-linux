#!/usr/bin/env bash
# Stage 4: build a bootable (BIOS + USB hybrid) ISO with isolinux + xorriso.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require xorriso

[ -f "$KERNEL_IMAGE" ] || die "kernel missing (run scripts/01-kernel.sh)"
[ -f "$INITRAMFS" ]    || die "initramfs missing (run scripts/03-rootfs.sh)"

# Locate the syslinux/isolinux bits, which live in different places per distro.
find_file() {
  local name="$1"; shift
  local d
  for d in "$@"; do
    if [ -f "${d}/${name}" ]; then printf '%s\n' "${d}/${name}"; return 0; fi
  done
  return 1
}
SYS_DIRS="/usr/lib/ISOLINUX /usr/lib/syslinux /usr/lib/syslinux/bios /usr/share/syslinux"
MOD_DIRS="/usr/lib/syslinux/modules/bios /usr/lib/syslinux/bios /usr/share/syslinux /usr/lib/syslinux"

ISOLINUX_BIN="$(find_file isolinux.bin $SYS_DIRS)"      || die "isolinux.bin not found (install isolinux/syslinux)"
ISOHDPFX="$(find_file isohdpfx.bin $SYS_DIRS $MOD_DIRS)" || die "isohdpfx.bin not found (install syslinux)"
LDLINUX="$(find_file ldlinux.c32 $MOD_DIRS)"            || die "ldlinux.c32 not found"
MENU_C32="$(find_file menu.c32 $MOD_DIRS)"              || die "menu.c32 not found"
LIBUTIL="$(find_file libutil.c32 $MOD_DIRS)" || true    # needed by menu.c32 on some builds

log "staging ISO tree"
ISO_ROOT="${BUILD_DIR}/iso"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot" "$ISO_ROOT/isolinux"

cp -f "$KERNEL_IMAGE" "$ISO_ROOT/boot/vmlinuz"
cp -f "$INITRAMFS"    "$ISO_ROOT/boot/initramfs.gz"
cp -f "${PROJECT_ROOT}/config/isolinux.cfg" "$ISO_ROOT/isolinux/isolinux.cfg"
cp -f "$ISOLINUX_BIN" "$LDLINUX" "$MENU_C32" "$ISO_ROOT/isolinux/"
[ -n "${LIBUTIL:-}" ] && cp -f "$LIBUTIL" "$ISO_ROOT/isolinux/"

mkdir -p "$OUTPUT_DIR"
log "writing ${ISO_IMAGE}"
xorriso -as mkisofs \
  -o "$ISO_IMAGE" \
  -V "ZENITH_LINUX" \
  -b isolinux/isolinux.bin \
  -c isolinux/boot.cat \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  -isohybrid-mbr "$ISOHDPFX" \
  "$ISO_ROOT"

ok "ISO built: ${ISO_IMAGE} ($(du -h "$ISO_IMAGE" | cut -f1))"
echo
echo "Test it:  qemu-system-x86_64 -m 512 -cdrom ${ISO_IMAGE}"
echo "Serial :  qemu-system-x86_64 -m 512 -cdrom ${ISO_IMAGE} -nographic"
