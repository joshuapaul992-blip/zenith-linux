#!/usr/bin/env bash
# GUI stage: assemble the bootable live ISO (kernel + live initramfs + squashfs).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

require xorriso
[ -f "$KERNEL_IMAGE" ] || die "GUI kernel missing (run the kernel stage)"
[ -f "$INITRAMFS" ]    || die "live initramfs missing (run scripts/gui/initramfs.sh)"
[ -f "$SQUASHFS" ]     || die "squashfs missing (run scripts/gui/squashfs.sh)"

find_file() { local n="$1"; shift; local d; for d in "$@"; do [ -f "${d}/${n}" ] && { echo "${d}/${n}"; return 0; }; done; return 1; }
SYS_DIRS="/usr/lib/ISOLINUX /usr/lib/syslinux /usr/lib/syslinux/bios /usr/share/syslinux"
MOD_DIRS="/usr/lib/syslinux/modules/bios /usr/lib/syslinux/bios /usr/share/syslinux /usr/lib/syslinux"

ISOLINUX_BIN="$(find_file isolinux.bin $SYS_DIRS)"       || die "isolinux.bin not found"
ISOHDPFX="$(find_file isohdpfx.bin $SYS_DIRS $MOD_DIRS)" || die "isohdpfx.bin not found"
LDLINUX="$(find_file ldlinux.c32 $MOD_DIRS)"             || die "ldlinux.c32 not found"
MENU_C32="$(find_file menu.c32 $MOD_DIRS)"               || die "menu.c32 not found"
LIBUTIL="$(find_file libutil.c32 $MOD_DIRS)" || true

log "staging GUI ISO tree"
ISO_ROOT="${BUILD_DIR}/iso"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot" "$ISO_ROOT/isolinux" "$ISO_ROOT/zenith"

cp -f "$KERNEL_IMAGE" "$ISO_ROOT/boot/vmlinuz"
cp -f "$INITRAMFS"    "$ISO_ROOT/boot/initramfs.gz"
cp -f "$SQUASHFS"     "$ISO_ROOT/zenith/rootfs.squashfs"
cp -f "${PROJECT_ROOT}/config/isolinux-gui.cfg" "$ISO_ROOT/isolinux/isolinux.cfg"
cp -f "$ISOLINUX_BIN" "$LDLINUX" "$MENU_C32" "$ISO_ROOT/isolinux/"
[ -n "${LIBUTIL:-}" ] && cp -f "$LIBUTIL" "$ISO_ROOT/isolinux/"

mkdir -p "$OUTPUT_DIR"
log "writing ${GUI_ISO}"
xorriso -as mkisofs \
  -o "$GUI_ISO" \
  -V "ZENITH_GUI" \
  -b isolinux/isolinux.bin \
  -c isolinux/boot.cat \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  -isohybrid-mbr "$ISOHDPFX" \
  "$ISO_ROOT"

ok "GUI ISO built: ${GUI_ISO} ($(du -h "$GUI_ISO" | cut -f1))"
echo
echo "Test it:  qemu-system-x86_64 -m 1024 -cdrom ${GUI_ISO} -vga std"
