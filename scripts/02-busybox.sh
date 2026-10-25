#!/usr/bin/env bash
# Stage 2: fetch and build a statically-linked BusyBox (the whole userland).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require gcc make bzip2

mkdir -p "$BUILD_DIR"
fetch "$BUSYBOX_URL" "$BUSYBOX_TARBALL"

if [ ! -d "$BUSYBOX_SRC" ]; then
  log "extracting busybox-${BUSYBOX_VERSION}"
  tar -C "$BUILD_DIR" -xf "${CACHE_DIR}/${BUSYBOX_TARBALL}"
fi

cd "$BUSYBOX_SRC"

log "configuring busybox (defconfig + static)"
make defconfig >/dev/null
# Build one self-contained static binary.
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
# tc(8) fails to build against modern kernel headers; it is not needed here.
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
# PIE + fully static don't mix on all toolchains; keep it plain static.
sed -i 's/^CONFIG_PIE=y/# CONFIG_PIE is not set/' .config
make olddefconfig >/dev/null

log "building busybox with ${JOBS} jobs"
make -j"${JOBS}"
make install >/dev/null   # populates ./_install with the symlink farm

if file busybox 2>/dev/null | grep -q 'dynamically linked'; then
  warn "busybox is dynamically linked; static build may have fallen back"
fi
ok "busybox built: $(du -h busybox | cut -f1) static binary"
