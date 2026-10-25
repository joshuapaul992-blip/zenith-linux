#!/usr/bin/env bash
# GUI stage: build the live root filesystem.
#
#   Base + X server : Ubuntu 26.10 (ubuntu-base) + its Xorg packages.
#   Desktop         : the genuine Damn Small Linux 4.4.10 filesystem at
#                     /opt/dsl, run in a chroot against Ubuntu's X server.
#
# The DSL binaries are 32-bit (i386); we inject a modern 32-bit glibc into the
# DSL tree so its 2008 loader's vDSO assertion goes away, while keeping DSL's
# own old X11/GTK libraries. Needs root (chroot/apt). On CI, run under sudo.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

[ "$(id -u)" -eq 0 ] || die "GUI rootfs build needs root (chroot/apt). Re-run with sudo."
require curl tar chroot xorriso extract_compressed_fs
[ -x "${BUSYBOX_SRC}/busybox" ] || die "static busybox not built yet (run the busybox stage first)"

XORG_PKGS="xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-video-vesa \
xserver-xorg-input-libinput xinit x11-xserver-utils xserver-xorg-legacy xauth \
xfonts-base xfonts-75dpi xfonts-100dpi"
BASE_PKGS="udev kmod ca-certificates dhcpcd-base iproute2"
I386_PKGS="libc6:i386 libx11-6:i386 libxpm4:i386 libxext6:i386 libxi6:i386 \
libsm6:i386 libice6:i386 libxau6:i386 libxdmcp6:i386"

# --- 1. Extract the genuine DSL 4.4.10 filesystem ---------------------------
if [ ! -d "$DSL_ROOT" ]; then
  fetch "$DSL_URL" "$DSL_ISO"
  log "extracting DSL ISO (cloop -> inner iso -> rootfs)"
  work="${BUILD_DIR}/dsl-work"; rm -rf "$work"; mkdir -p "$work"
  xorriso -osirrox on -indev "${CACHE_DIR}/${DSL_ISO}" -extract / "$work/iso" >/dev/null 2>&1
  cloop="$(find "$work/iso" -name KNOPPIX -type f | head -1)"
  [ -n "$cloop" ] || die "KNOPPIX cloop not found in DSL ISO"
  extract_compressed_fs "$cloop" "$work/inner.img" >/dev/null 2>&1
  rm -rf "$DSL_ROOT"
  xorriso -osirrox on -indev "$work/inner.img" -extract / "$DSL_ROOT" >/dev/null 2>&1
  rm -rf "$work"
fi
[ -x "$DSL_ROOT/usr/bin/jwm" ] || die "DSL extraction failed (no jwm)"
ok "DSL filesystem ready: $(du -sh "$DSL_ROOT" | cut -f1)"

# --- 2. Ubuntu base rootfs --------------------------------------------------
fetch "$UBUNTU_URL" "$UBUNTU_TARBALL"
log "extracting Ubuntu ${UBUNTU_VERSION} base"
rm -rf "$GUI_ROOTFS"; mkdir -p "$GUI_ROOTFS"
tar -C "$GUI_ROOTFS" -xzf "${CACHE_DIR}/${UBUNTU_TARBALL}"
cp -f /etc/resolv.conf "$GUI_ROOTFS/etc/resolv.conf" 2>/dev/null || true

# Proxy passthrough for apt in sandboxed build envs (a no-op on normal CI).
APT_ENV=""
for ca in /root/.ccr/ca-bundle.crt "${ZENITH_PROXY_CA:-}"; do
  if [ -n "$ca" ] && [ -f "$ca" ]; then
    cp -f "$ca" "$GUI_ROOTFS/usr/local/share/ca-certificates/zenith-proxy.crt" 2>/dev/null || true
    APT_ENV="export http_proxy='${HTTP_PROXY:-${http_proxy:-}}' https_proxy='${HTTPS_PROXY:-${https_proxy:-}}';"
    break
  fi
done

cleanup() {
  for m in proc sys dev/pts dev; do umount "$GUI_ROOTFS/$m" 2>/dev/null || true; done
}
trap cleanup EXIT
mount -t proc none "$GUI_ROOTFS/proc"
mount --bind /sys  "$GUI_ROOTFS/sys"
mount --bind /dev  "$GUI_ROOTFS/dev"
mount -t devpts none "$GUI_ROOTFS/dev/pts" 2>/dev/null || true

log "installing Ubuntu Xorg + i386 libraries via apt"
chroot "$GUI_ROOTFS" /bin/bash -c "
  set -e
  ${APT_ENV}
  export DEBIAN_FRONTEND=noninteractive
  update-ca-certificates 2>/dev/null || true
  dpkg --add-architecture i386
  apt-get update
  apt-get install -y --no-install-recommends ${BASE_PKGS} ${XORG_PKGS} ${I386_PKGS}
  apt-get clean
"
cleanup; trap - EXIT
rm -rf "$GUI_ROOTFS"/var/lib/apt/lists/* "$GUI_ROOTFS"/var/cache/apt/archives/*.deb 2>/dev/null || true
# Remove apt/dpkg lock files left from the build: baked into the read-only
# squashfs they break apt/dpkg on the live system (copy-up/flock failures,
# "lock was locked by another process", "Permission denied"). Fresh ones are
# created on the writable overlay at runtime.
rm -f "$GUI_ROOTFS"/var/lib/dpkg/lock "$GUI_ROOTFS"/var/lib/dpkg/lock-frontend \
      "$GUI_ROOTFS"/var/lib/apt/lists/lock "$GUI_ROOTFS"/var/cache/apt/archives/lock 2>/dev/null || true

# --- 3. Overlay the DSL filesystem at /opt/dsl ------------------------------
log "overlaying DSL 4.4.10 at /opt/dsl"
rm -rf "$GUI_ROOTFS/opt/dsl"; mkdir -p "$GUI_ROOTFS/opt/dsl"
cp -a "$DSL_ROOT/." "$GUI_ROOTFS/opt/dsl/"

# Inject a modern 32-bit glibc into the DSL tree (fixes the old loader's vDSO
# assertion); keep DSL's old X11/GTK libraries.
log "injecting modern 32-bit glibc into /opt/dsl"
for lib in ld-linux.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 \
           librt.so.1 libresolv.so.2 libutil.so.1; do
  src="$GUI_ROOTFS/lib/i386-linux-gnu/$lib"
  [ -e "$src" ] || src="$(readlink -f "$GUI_ROOTFS/lib/i386-linux-gnu/$lib" 2>/dev/null)"
  [ -f "$src" ] && cp -f "$src" "$GUI_ROOTFS/opt/dsl/lib/$lib"
done
# DSL's own old X11/GTK libraries work fine against the injected modern glibc
# (verified), so nothing else needs replacing.

# DSL symlinks /tmp -> /var/tmp. That absolute symlink resolves OUTSIDE the
# chroot, so bind-mounting the X socket onto /opt/dsl/tmp/.X11-unix lands on the
# host's /var/tmp and the chrooted client sees an empty /tmp. Replace it with a
# real directory so the X unix socket is actually shared into the chroot.
rm -f "$GUI_ROOTFS/opt/dsl/tmp"
mkdir -p "$GUI_ROOTFS/opt/dsl/tmp"; chmod 1777 "$GUI_ROOTFS/opt/dsl/tmp"
# Bind-mount points for the chroot session (DSL's tree lacks /sys).
mkdir -p "$GUI_ROOTFS/opt/dsl/proc" "$GUI_ROOTFS/opt/dsl/sys" "$GUI_ROOTFS/opt/dsl/dev"

# Seed the DSL home from its skeleton so the real configs are in place.
mkdir -p "$GUI_ROOTFS/opt/dsl/home/dsl"
cp -a "$GUI_ROOTFS/opt/dsl/etc/skel/." "$GUI_ROOTFS/opt/dsl/home/dsl/" 2>/dev/null || true

# --- 4. Zenith init + session overlay ---------------------------------------
log "overlaying Zenith init + DSL session launcher"
cp -a "${PROJECT_ROOT}/gui-overlay/." "$GUI_ROOTFS/"
install -D -m0755 "${BUSYBOX_SRC}/busybox" "$GUI_ROOTFS/sbin/init"
chmod 0755 "$GUI_ROOTFS/etc/zenith/rcS" "$GUI_ROOTFS/etc/zenith/xstart"
chmod 0755 "$GUI_ROOTFS/opt/dsl/dsl-session.sh" 2>/dev/null || true
mkdir -p "$GUI_ROOTFS/var/log" "$GUI_ROOTFS/tmp/.X11-unix"

# Allow Xorg to start on a VT as root (no logind in this live system).
mkdir -p "$GUI_ROOTFS/etc/X11"
printf 'allowed_users=anybody\nneeds_root_rights=yes\n' > "$GUI_ROOTFS/etc/X11/Xwrapper.config"

ok "GUI rootfs ready: $(du -sh --exclude=proc --exclude=sys --exclude=dev "$GUI_ROOTFS" 2>/dev/null | cut -f1)"
