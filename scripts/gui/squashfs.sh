#!/usr/bin/env bash
# GUI stage: compress the live root filesystem into a SquashFS image.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

require mksquashfs
[ -d "$GUI_ROOTFS" ] || die "GUI rootfs missing (run scripts/gui/rootfs.sh)"

log "building squashfs (xz)"
rm -f "$SQUASHFS"
mksquashfs "$GUI_ROOTFS" "$SQUASHFS" \
  -comp xz -noappend -no-progress \
  -e proc sys dev var/cache/apk

ok "squashfs built: ${SQUASHFS} ($(du -h "$SQUASHFS" | cut -f1))"
