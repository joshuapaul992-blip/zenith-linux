# shellcheck shell=bash
# Shared helpers and configuration for the Zenith Linux build.
# Sourced by build.sh and every scripts/NN-*.sh stage.

set -euo pipefail

# --- Version pins ---------------------------------------------------------
# Zenith Linux targets a modern kernel with a Damn-Small-Linux-style userland.
: "${KERNEL_VERSION:=6.18}"
: "${BUSYBOX_VERSION:=1.37.0}"
: "${ZENITH_VERSION:=0.1.0}"

# --- Layout ---------------------------------------------------------------
# PROJECT_ROOT is the repo root regardless of where a script is invoked from.
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${BUILD_DIR:=${PROJECT_ROOT}/build}"       # all intermediate artifacts (gitignored)
: "${CACHE_DIR:=${BUILD_DIR}/cache}"          # downloaded tarballs (gitignored)
: "${OUTPUT_DIR:=${PROJECT_ROOT}/output}"     # final ISO (gitignored)
: "${ROOTFS_STAGE:=${BUILD_DIR}/rootfs}"      # assembled root filesystem
: "${JOBS:=$(nproc 2>/dev/null || echo 2)}"

KERNEL_SRC="${BUILD_DIR}/linux-${KERNEL_VERSION}"
BUSYBOX_SRC="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}"

KERNEL_TARBALL="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${KERNEL_TARBALL}"
BUSYBOX_TARBALL="busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TARBALL}"

# Final build products.
KERNEL_IMAGE="${BUILD_DIR}/vmlinuz"
INITRAMFS="${BUILD_DIR}/initramfs.cpio.gz"
ISO_IMAGE="${OUTPUT_DIR}/zenith_linux.iso"

# --- Logging --------------------------------------------------------------
_c_blue=$'\033[1;34m'; _c_green=$'\033[1;32m'; _c_yellow=$'\033[1;33m'
_c_red=$'\033[1;31m'; _c_reset=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$_c_blue"  "$_c_reset" "$*"; }
ok()   { printf '%s ok%s %s\n' "$_c_green" "$_c_reset" "$*"; }
warn() { printf '%swarn%s %s\n' "$_c_yellow" "$_c_reset" "$*" >&2; }
die()  { printf '%serr%s %s\n'  "$_c_red" "$_c_reset" "$*" >&2; exit 1; }

# --- Download with on-disk cache -----------------------------------------
# fetch <url> <dest-filename-in-cache>
fetch() {
  local url="$1" name="$2" dest="${CACHE_DIR}/$2"
  mkdir -p "$CACHE_DIR"
  if [ -s "$dest" ]; then
    ok "cached ${name}"
    return 0
  fi
  log "downloading ${name}"
  # Retry with exponential backoff to survive flaky networks.
  local delay=2 attempt
  for attempt in 1 2 3 4 5; do
    if curl -fL --connect-timeout 30 -o "${dest}.part" "$url"; then
      mv "${dest}.part" "$dest"
      ok "downloaded ${name}"
      return 0
    fi
    warn "download failed (attempt ${attempt}), retrying in ${delay}s"
    sleep "$delay"; delay=$((delay * 2))
  done
  die "could not download ${url}"
}

require() {
  local missing=0 t
  for t in "$@"; do
    command -v "$t" >/dev/null 2>&1 || { warn "missing required tool: $t"; missing=1; }
  done
  [ "$missing" -eq 0 ] || die "install the missing tools first (try: scripts/00-deps.sh)"
}
