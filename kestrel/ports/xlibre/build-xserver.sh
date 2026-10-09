#!/bin/sh
# ports/xlibre/build-xserver.sh XLIBRE_SRC SYSROOT BUILDDIR
#
# Configure and build XLibre for Kestrel with musl against the sysroot made
# by build-sysroot.sh. Only what Kestrel can run today is enabled: software
# rendering (fb), no GL/DRI, no udev/logind/libseat, no input thread, no
# MIT-SHM (no shared memory yet), no XDMCP, Unix-socket listening only,
# built-in fonts, a precompiled keymap.
set -eu
SRC=$(cd "$1" && pwd)
SYSROOT=$(cd "$2" && pwd)
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
sed "s|@SYSROOT@|$SYSROOT|g" "$HERE/kestrel-cross.txt.in" > "$OUT/kestrel-cross.txt"
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_PATH=
# the Kestrel DDX (hw/kestrel), once ports/xlibre/apply-patches.sh added it
KOPT=$(grep -q "option('kestrel'" "$SRC/meson_options.txt" && echo -Dkestrel=true || true)
if [ ! -f "$OUT/build/build.ninja" ]; then
    meson setup "$OUT/build" "$SRC" --cross-file "$OUT/kestrel-cross.txt" \
        -Dxorg=false -Dxephyr=false -Dxnest=false -Dxvfb=true -Dxwin=false -Dxquartz=false \
        $KOPT \
        -Dglamor=false -Dglx=false -Dxdmcp=false -Dxdm-auth-1=false \
        -Dudev=false -Dudev_kms=false -Dsystemd_logind=false -Dseatd_libseat=false \
        -Dinput_thread=false -Dmitshm=false -Ddri1=false -Ddri2=false -Ddri3=false -Ddrm=false \
        -Dpciaccess=false -Dxselinux=false -Dsha1=libsha1 -Dtests=false -Ddocs=false \
        -Dxvmc=false -Ddga=false -Dagp=false -Dint10=false -Dlinux_apm=false -Dlinux_acpi=false \
        -Dvgahw=false -Ddefault_font_path=built-ins -Dlisten_tcp=false \
        -Dxkb_dir=/boot/share/X11/xkb -Dxkb_output_dir=/tmp -Dlog_dir=/tmp
fi
ninja -C "$OUT/build"
