#!/usr/bin/env bash
# GUI stage: build the small live initramfs that finds the media, mounts the
# SquashFS, stacks a tmpfs overlay, and switch_root's into it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

require cpio gzip find
[ -x "${BUSYBOX_SRC}/busybox" ] || die "static busybox not built yet (run the busybox stage first)"

STAGE="${BUILD_DIR}/live-initramfs"
log "assembling live initramfs"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,sbin,proc,sys,dev,mnt,newroot}

cp -f "${BUSYBOX_SRC}/busybox" "$STAGE/bin/busybox"
# Relative applet symlinks (applet -> busybox) inside /bin, so they resolve at
# boot regardless of the build path. busybox picks the applet from argv[0].
# (The live /init calls everything as `busybox <applet>` anyway; these just make
# the emergency rescue shell fully usable.)
( cd "$STAGE/bin"
  for app in $(./busybox --list); do
    [ "$app" = busybox ] && continue
    ln -sf busybox "$app"
  done )

install -m0755 "${PROJECT_ROOT}/rootfs-live/init" "$STAGE/init"

[ -e "$STAGE/dev/console" ] || mknod -m600 "$STAGE/dev/console" c 5 1 2>/dev/null || true
[ -e "$STAGE/dev/null" ]    || mknod -m666 "$STAGE/dev/null"    c 1 3 2>/dev/null || true

( cd "$STAGE" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$INITRAMFS"
ok "live initramfs built: ${INITRAMFS} ($(du -h "$INITRAMFS" | cut -f1))"
