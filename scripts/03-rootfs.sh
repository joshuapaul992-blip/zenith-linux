#!/usr/bin/env bash
# Stage 3: assemble the root filesystem and pack it into a gzip'd cpio initramfs.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require cpio gzip find

[ -x "${BUSYBOX_SRC}/busybox" ] || die "busybox not built yet (run scripts/02-busybox.sh)"

log "assembling root filesystem"
rm -rf "$ROOTFS_STAGE"
mkdir -p "$ROOTFS_STAGE"/{bin,sbin,proc,sys,dev,tmp,run,root,mnt,usr/bin,usr/sbin}

# 1. Install the static busybox and its full symlink farm (sh, mount, ls, ...).
cp -a "${BUSYBOX_SRC}/_install/." "$ROOTFS_STAGE/"
cp -f "${BUSYBOX_SRC}/busybox" "$ROOTFS_STAGE/bin/busybox"

# 2. Overlay our tracked skeleton (init, /etc, ...).
cp -a "${PROJECT_ROOT}/rootfs/." "$ROOTFS_STAGE/"

# 3. Permissions: PID 1 and startup scripts must be executable.
chmod 0755 "$ROOTFS_STAGE/init" "$ROOTFS_STAGE/etc/init.d/rcS"

# 4. A couple of essential device nodes in case devtmpfs is unavailable early.
#    (Harmless when the kernel's devtmpfs auto-populates /dev.)
mkdir -p "$ROOTFS_STAGE/dev"
[ -e "$ROOTFS_STAGE/dev/console" ] || mknod -m 600 "$ROOTFS_STAGE/dev/console" c 5 1 2>/dev/null || true
[ -e "$ROOTFS_STAGE/dev/null" ]    || mknod -m 666 "$ROOTFS_STAGE/dev/null"    c 1 3 2>/dev/null || true

log "packing initramfs"
( cd "$ROOTFS_STAGE" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$INITRAMFS"

ok "initramfs built: ${INITRAMFS} ($(du -h "$INITRAMFS" | cut -f1))"
