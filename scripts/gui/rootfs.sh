#!/usr/bin/env bash
# GUI stage: build the Alpine-based live root filesystem with the DSL-style
# X desktop (Xorg + JWM/Fluxbox + xfe + conky, themed like DSL 4.4.10).
#
# Requires root (chroot + apk). On CI, run under sudo.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

[ "$(id -u)" -eq 0 ] || die "GUI rootfs build needs root (chroot/apk). Re-run with sudo."
require curl tar chroot
[ -x "${BUSYBOX_SRC}/busybox" ] || die "static busybox not built yet (run the busybox stage first)"

# Package set validated against Alpine 3.24: Xorg + generic video drivers,
# input, eudev, both window managers, a light file manager, monitor, terminal,
# wallpaper setter, and fonts.
GUI_PKGS="xorg-server xinit xauth \
xf86-video-vesa xf86-video-fbdev mesa-dri-gallium xf86-input-libinput \
xrandr xsetroot xset setxkbmap xkeyboard-config \
eudev jwm fluxbox xfe conky xterm feh \
font-misc-misc font-cursor-misc ttf-dejavu"

fetch "$ALPINE_URL" "$ALPINE_TARBALL"
if command -v sha256sum >/dev/null 2>&1; then
  echo "${ALPINE_SHA256}  ${CACHE_DIR}/${ALPINE_TARBALL}" | sha256sum -c - \
    || die "Alpine minirootfs checksum mismatch"
fi

log "extracting Alpine minirootfs"
rm -rf "$GUI_ROOTFS"; mkdir -p "$GUI_ROOTFS"
tar -C "$GUI_ROOTFS" -xzf "${CACHE_DIR}/${ALPINE_TARBALL}"

cp -f /etc/resolv.conf "$GUI_ROOTFS/etc/resolv.conf" 2>/dev/null || true

# Pass a proxy CA through to apk when one is present (needed inside sandboxed
# build environments; a no-op on normal CI where public HTTPS is trusted).
APK_ENV=""
for ca in /root/.ccr/ca-bundle.crt "${ZENITH_PROXY_CA:-}"; do
  if [ -n "$ca" ] && [ -f "$ca" ]; then
    cp -f "$ca" "$GUI_ROOTFS/zenith-proxy-ca.crt"
    APK_ENV="export SSL_CERT_FILE=/zenith-proxy-ca.crt HTTPS_PROXY='${HTTPS_PROXY:-}' https_proxy='${HTTPS_PROXY:-}';"
    break
  fi
done

cleanup() {
  umount "$GUI_ROOTFS/proc" 2>/dev/null || true
  umount "$GUI_ROOTFS/dev"  2>/dev/null || true
}
trap cleanup EXIT
mount -t proc none "$GUI_ROOTFS/proc"
mount --bind /dev  "$GUI_ROOTFS/dev"

log "installing X desktop packages via apk"
chroot "$GUI_ROOTFS" /bin/sh -c "${APK_ENV} apk update && apk add ${GUI_PKGS}"

cleanup; trap - EXIT

# Drop the apk cache and the temporary proxy CA from the final image.
find "$GUI_ROOTFS/var/cache/apk" -mindepth 1 -delete 2>/dev/null || true
rm -f "$GUI_ROOTFS/zenith-proxy-ca.crt"

log "overlaying Zenith desktop theme + init"
cp -a "${PROJECT_ROOT}/gui-overlay/." "$GUI_ROOTFS/"

# busybox init as PID 1 (our static build guarantees the init applet).
install -D -m0755 "${BUSYBOX_SRC}/busybox" "$GUI_ROOTFS/sbin/init"

chmod 0755 "$GUI_ROOTFS/etc/zenith/rcS" "$GUI_ROOTFS/etc/zenith/xstart" \
           "$GUI_ROOTFS/etc/zenith/setwm" "$GUI_ROOTFS/etc/X11/xinit/xinitrc"
mkdir -p "$GUI_ROOTFS/var/log"

ok "GUI rootfs ready: ${GUI_ROOTFS} ($(du -sh --exclude=proc --exclude=sys --exclude=dev "$GUI_ROOTFS" 2>/dev/null | cut -f1))"
