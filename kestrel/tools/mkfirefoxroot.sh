#!/bin/sh
# tools/mkfirefoxroot.sh OUT.tar FIREFOX_DIR [DEBCACHE]
#
# Build a root file system of Ubuntu 24.04 (noble) runtime packages with
# Firefox in /opt/firefox, as a tarball for
#
#   make usbimg ROOTFS_BASE=OUT.tar ROOTDISK_MB=2400
#
# Runs on an Ubuntu 24.04 host (apt-get download with noble's sources) as
# root (chroot runs the cache tools). FIREFOX_DIR is an unpacked Linux
# x86_64 Firefox: the directory with firefox-bin, libxul.so and omni.ja
# (Mozilla's tarball, or the conda-forge package's bin/FirefoxApp).
#
# What the root holds:
# - glibc, bash, coreutils and the usual tools;
# - GTK 3, GLib, Pango, cairo, fontconfig with DejaVu fonts, Mesa (llvmpipe
#   through EGL), the X client libraries and tools (xdpyinfo, xeyes,
#   glxinfo/eglinfo), D-Bus, gtk3-demo;
# - sound: ALSA with its OSS plugin and an asound.conf that sends ALSA
#   programs to Kestrel's /dev/dsp and /dev/mixer, and PulseAudio with
#   module-oss on /dev/dsp (/etc/pulse/kestrel.pa), because Firefox's
#   official builds play only through PulseAudio (or JACK);
# - /usr/local/bin/firefox, which starts PulseAudio if it is not running
#   and Firefox with the sandbox off (Kestrel has no seccomp or namespaces
#   yet), and an IceWM menu for root.
set -e
OUT=$1; FF=$2; CACHE=${3:-$(dirname "$OUT")/debcache}
[ -n "$OUT" ] && [ -f "$FF/firefox-bin" ] || { echo "usage: $0 OUT.tar FIREFOX_DIR [DEBCACHE]"; exit 2; }
ROOT=$(mktemp -d "${TMPDIR:-/tmp}/ffroot.XXXXXX")
trap 'rm -rf "${ROOT:?}"' EXIT

PKGS="bash coreutils findutils grep sed gawk procps util-linux xz-utils zstd less
      libgtk-3-0t64 libgtk-3-bin gtk-3-examples libasound2t64 alsa-utils libdbus-1-3 dbus-daemon dbus-x11
      libgl1-mesa-dri libegl1 libegl-mesa0 mesa-utils x11-utils x11-apps x11-xserver-utils
      fonts-dejavu-core fontconfig hicolor-icon-theme adwaita-icon-theme librsvg2-common shared-mime-info
      libpci3 libx11-xcb1 libxcomposite1 libxdamage1 libxrandr2 libxcursor1 libxi6 libxtst6 libxss1
      libdbus-glib-1-2 pulseaudio pulseaudio-utils libpulse0 libasound2-plugins"

echo "mkfirefoxroot: resolving packages"
mkdir -p "$CACHE"
pk=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
         --no-replaces --no-enhances $PKGS 2>/dev/null | grep -E '^[a-zA-Z0-9]' | sed 's/:any$//' | sort -u)
real=""
for p in $pk; do apt-cache show --no-all-versions "$p" >/dev/null 2>&1 && real="$real $p"; done
echo "mkfirefoxroot: downloading $(echo $real | wc -w) packages into $CACHE"
(cd "$CACHE" && for p in $real; do ls "${p}"_*.deb >/dev/null 2>&1 || apt-get download -q "$p" >/dev/null 2>&1 || echo "  skipped $p"; done)

echo "mkfirefoxroot: unpacking"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/usr/lib" "$ROOT/usr/lib64"
for d in bin sbin lib lib64; do ln -s usr/$d "$ROOT/$d"; done     # merged /usr, before any package
for p in $real; do
    for d in "$CACHE/${p}"_*.deb; do
        [ -f "$d" ] || continue
        dpkg-deb --fsys-tarfile "$d" | tar -x --keep-directory-symlink -C "$ROOT" 2>/dev/null || echo "  warning: $(basename "$d")"
    done
done

echo "mkfirefoxroot: configuring"
cat > "$ROOT/etc/asound.conf" <<'EOF'
# ALSA programs (aplay, speaker-test) play through Kestrel's OSS devices
pcm.!default { type oss device /dev/dsp }
ctl.!default { type oss device /dev/mixer }
EOF
cat > "$ROOT/etc/pulse/kestrel.pa" <<'EOF'
# PulseAudio on Kestrel: one sink, the kernel's OSS device (HD Audio)
load-module module-oss device=/dev/dsp sink_name=kestrel playback=1 record=0 mmap=0
load-module module-native-protocol-unix auth-anonymous=1
load-module module-always-sink
set-default-sink kestrel
EOF
sed -i 's/^; autospawn = yes/autospawn = no/' "$ROOT/etc/pulse/client.conf"
python3 -c "import uuid; print(uuid.uuid4().hex)" > "$ROOT/etc/machine-id"
mkdir -p "$ROOT/var/lib/dbus" "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" "$ROOT/tmp" "$ROOT/root"
cp "$ROOT/etc/machine-id" "$ROOT/var/lib/dbus/machine-id"
chroot "$ROOT" /usr/bin/fc-cache -f
chroot "$ROOT" /usr/lib/x86_64-linux-gnu/glib-2.0/glib-compile-schemas /usr/share/glib-2.0/schemas
chroot "$ROOT" /usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders --update-cache
chroot "$ROOT" /usr/bin/update-mime-database /usr/share/mime
for t in hicolor Adwaita; do
    [ -d "$ROOT/usr/share/icons/$t" ] && chroot "$ROOT" /usr/bin/gtk-update-icon-cache -f -q /usr/share/icons/$t || true
done

echo "mkfirefoxroot: Firefox from $FF"
mkdir -p "$ROOT/opt"
cp -a "$FF" "$ROOT/opt/firefox"
mkdir -p "$ROOT/opt/firefox/distribution"
cat > "$ROOT/opt/firefox/distribution/policies.json" <<'EOF'
{
  "policies": {
    "SkipTermsOfUse": true,
    "DisableTelemetry": true,
    "DontCheckDefaultBrowser": true,
    "OverrideFirstRunPage": "",
    "OverridePostUpdatePage": "",
    "DisableAppUpdate": true
  }
}
EOF
cat > "$ROOT/opt/firefox/defaults/pref/kestrel.js" <<'EOF'
// Kestrel: let Web Audio start without a click, and skip first-run prompts
pref("media.autoplay.block-webaudio", false);
pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
pref("browser.shell.checkDefaultBrowser", false);
pref("browser.aboutwelcome.enabled", false);
EOF
mkdir -p "$ROOT/usr/local/bin"
cat > "$ROOT/usr/local/bin/firefox" <<'EOF'
#!/bin/sh
# Firefox on Kestrel: its sandboxes need seccomp-bpf and namespaces, which
# Kestrel does not have yet, so they are off.
export MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1 \
       MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 MOZ_DISABLE_UTILITY_SANDBOX=1 MOZ_DISABLE_GPU_SANDBOX=1 \
       MOZ_CRASHREPORTER_DISABLE=1
export DISPLAY=${DISPLAY:-:0}
[ "$HOME" = / ] && [ "$(id -u)" = 0 ] && export HOME=/root     # kestrel.exec programs start with HOME=/
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
pactl info >/dev/null 2>&1 ||
    pulseaudio --daemonize=yes -n -F /etc/pulse/kestrel.pa --exit-idle-time=-1 --log-target=syslog 2>/dev/null
exec /opt/firefox/firefox-bin "$@"
EOF
chmod 755 "$ROOT/usr/local/bin/firefox"
mkdir -p "$ROOT/root/.icewm"
cat > "$ROOT/root/.icewm/menu" <<'EOF'
prog "Firefox" - /usr/local/bin/firefox
prog "XTerm" xterm xterm
prog "GTK 3 demo" - gtk3-demo
prog "xeyes" - xeyes
EOF
cp "$ROOT/root/.icewm/menu" "$ROOT/root/.icewm/toolbar"
ln -sfn root/.icewm "$ROOT/.icewm"                  # the desktop started at boot runs with HOME=/

echo "mkfirefoxroot: writing $OUT"
tar -C "$ROOT" --numeric-owner -cf "$OUT" .
echo "mkfirefoxroot: $(du -sh "$ROOT" | cut -f1) root in $OUT"
