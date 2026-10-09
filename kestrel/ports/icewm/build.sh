#!/bin/sh
# ports/icewm/build.sh ICEWM_SRC BUILD
#
# Build the IceWM window manager for Kestrel and install it into
# BUILD/icewm-root, laid out like the boot volume (/boot):
#
#   bin/icewm                       the window manager (static-PIE, musl, C++)
#   share/icewm/                    menus, key bindings, the default theme
#   etc/icewm/                      Kestrel settings (prefoverride: fonts, applets)
#                                   and menus (menu, programs)
#
# ICEWM_SRC  an unpacked IceWM 3.x release (Ubuntu's icewm_3.4.5.orig.tar.xz)
# BUILD      Kestrel's build directory; BUILD/xsysroot must already hold the
#            X libraries (`make xlibre`, ports/xlibre/build-sysroot.sh)
#
# Configuration: X core fonts (the server has no FreeType), XPM + libpng
# images, no i18n/NLS, no session management, RandR, Xinerama, FriBidi,
# freedesktop menus or sound. Only the `icewm` program itself is built:
# icewm-session, icewmbg and icewmtray start further processes, which needs
# fork/exec.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
mkdir -p "$2"
BUILD=$(cd "$2" && pwd)
SYSROOT=$BUILD/xsysroot
ROOT=$BUILD/icewm-root
OUT=$BUILD/icewm

[ -f "$SYSROOT/lib/libXpm.a" ] && [ -f "$SYSROOT/lib/libpng.a" ] || {
    echo "icewm: $SYSROOT lacks libXpm/libpng; run ports/xlibre/build-sysroot.sh first"; exit 1; }
"$HERE/../toolchain/install.sh" "$SYSROOT"

# IceWM's CMake lists libraries de-duplicated, which breaks static link
# order; hand the complete static set to the end of every link line.
LIBS=$("$SYSROOT/bin/kestrel-pkg-config" --libs xpm libpng xext xrender xcomposite xdamage xfixes x11)

cmake -S "$SRC" -B "$OUT" -DCMAKE_TOOLCHAIN_FILE="$SYSROOT/bin/kestrel.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD_LIBRARIES="$LIBS" \
    -DPREFIX=/boot -DCMAKE_INSTALL_PREFIX=/boot \
    -DCFGDIR=/boot/etc/icewm -DLIBDIR=/boot/share/icewm \
    -DCONFIG_IMLIB2=off -DCONFIG_XPM=on -DCONFIG_LIBPNG=on -DCONFIG_LIBJPEG=off \
    -DCONFIG_I18N=off -DENABLE_NLS=off -DCONFIG_SESSION=off -DCONFIG_XRANDR=off \
    -DCONFIG_COREFONTS=on -DCONFIG_XFREETYPE=off -DXINERAMA=off -DCONFIG_FRIBIDI=off \
    -DCONFIG_FDO_MENUS=off -DICESOUND=none -DBUILD_TESTING=off >"$BUILD/icewm.log" 2>&1 \
    || { tail -30 "$BUILD/icewm.log"; exit 1; }
make -C "$OUT" -j"${MAKEJ:-8}" icewm >>"$BUILD/icewm.log" 2>&1 \
    || { grep -E "error|undefined" "$BUILD/icewm.log" | head -30; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/share/icewm/themes" "$ROOT/etc/icewm"
echo "  STRIP   icewm"
strip -o "$ROOT/bin/icewm" "$OUT/icewm"
for f in keys menu programs toolbar winoptions; do
    cp "$OUT/lib/$f" "$ROOT/share/icewm/"
done
cp -r "$SRC/lib/taskbar" "$SRC/lib/icons" "$SRC/lib/ledclock" "$SRC/lib/mailbox" "$ROOT/share/icewm/"
cp -r "$SRC/lib/themes/default" "$ROOT/share/icewm/themes/"
cp "$HERE/prefoverride" "$HERE/menu" "$HERE/programs" "$ROOT/etc/icewm/"
touch "$ROOT/.stamp"
echo "icewm: installed into $ROOT"
