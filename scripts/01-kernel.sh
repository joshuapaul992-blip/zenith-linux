#!/usr/bin/env bash
# Stage 1: fetch and build the Linux kernel as a small, monolithic bzImage.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

require gcc make ld bc bison flex xz

mkdir -p "$BUILD_DIR"
fetch "$KERNEL_URL" "$KERNEL_TARBALL"

if [ ! -d "$KERNEL_SRC" ]; then
  log "extracting linux-${KERNEL_VERSION}"
  tar -C "$BUILD_DIR" -xf "${CACHE_DIR}/${KERNEL_TARBALL}"
fi

cd "$KERNEL_SRC"

# Start from the smallest possible config, then merge in our fragment.
log "configuring kernel (tinyconfig + Zenith fragment)"
make tinyconfig >/dev/null
./scripts/kconfig/merge_config.sh -m .config "${PROJECT_ROOT}/config/kernel.fragment" >/dev/null
# Resolve any dependencies the merge left as defaults, non-interactively.
make olddefconfig >/dev/null

log "building kernel with ${JOBS} jobs (this takes a while)"
make -j"${JOBS}" bzImage

cp -f arch/x86/boot/bzImage "$KERNEL_IMAGE"
size="$(du -h "$KERNEL_IMAGE" | cut -f1)"
ok "kernel built: ${KERNEL_IMAGE} (${size})"
