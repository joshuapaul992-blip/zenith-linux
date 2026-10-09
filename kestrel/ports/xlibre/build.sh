#!/bin/sh
# ports/xlibre/build.sh XLIBRE_SRC DEPS_SRC BUILD
#
# Build everything X11 for Kestrel and install it into BUILD/xlibre-root,
# laid out like the boot volume (/boot):
#
#   bin/Xkestrel                         the X server (static-PIE, musl)
#   bin/xdemo                            first test client (xcb)
#   share/X11/xkb/compiled/kestrel-default.xkm   precompiled us/pc105 keymap
#   share/X11/xkb/rules/evdev            rules file (names only; no xkbcomp)
#
# XLIBRE_SRC  a checkout of https://github.com/X11Libre/xserver (patched in
#             place by apply-patches.sh)
# DEPS_SRC    directory with the library source tarballs (see build-sysroot.sh)
# BUILD       Kestrel's build directory (sysroot goes to BUILD/xsysroot,
#             the server's meson tree to BUILD/xserver)
#
# Host needs: musl-gcc, meson, ninja, pkg-config, python3, xkbcomp, xkb-data.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
DEPS=$(cd "$2" && pwd)
mkdir -p "$3"
BUILD=$(cd "$3" && pwd)
SYSROOT=$BUILD/xsysroot
ROOT=$BUILD/xlibre-root
XKB=${XKB_DATA:-/usr/share/X11/xkb}

"$HERE/build-sysroot.sh" "$DEPS" "$SYSROOT"
"$HERE/apply-patches.sh" "$SRC"
"$HERE/build-xserver.sh" "$SRC" "$SYSROOT" "$BUILD/xserver"

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/share/X11/xkb/compiled" "$ROOT/share/X11/xkb/rules"

echo "  STRIP   Xkestrel"
strip -o "$ROOT/bin/Xkestrel" "$BUILD/xserver/build/hw/kestrel/Xkestrel"

echo "  MUSLCC  xdemo"
musl-gcc -std=gnu11 -O2 -Wall -fPIE -static-pie -I"$SYSROOT/include" \
    "$HERE/clients/xdemo.c" -L"$SYSROOT/lib" -lxcb -lXau -lXdmcp -o "$ROOT/bin/xdemo"
strip "$ROOT/bin/xdemo"

# The server has no xkbcomp to run (no fork/exec): compile the default
# keymap here. XLibre's built-in defaults are rules "evdev", model "pc105",
# layout "us"; patches/0001 hands this file to the server instead.
echo "  XKBCOMP kestrel-default.xkm"
cat > "$BUILD/kestrel-default.xkb" <<'EOF'
xkb_keymap {
    xkb_keycodes  { include "evdev+aliases(qwerty)" };
    xkb_types     { include "complete" };
    xkb_compat    { include "complete" };
    xkb_symbols   { include "pc+us+inet(evdev)" };
    xkb_geometry  { include "pc(pc105)" };
};
EOF
xkbcomp -w 0 -I"$XKB" -xkm "$BUILD/kestrel-default.xkb" "$ROOT/share/X11/xkb/compiled/kestrel-default.xkm"
cp "$XKB/rules/evdev" "$ROOT/share/X11/xkb/rules/evdev"

touch "$ROOT/.stamp"
echo "xlibre: installed into $ROOT"
