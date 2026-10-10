#!/bin/sh
# ports/xterm/build.sh XTERM_SRC BUILD
#
# xterm for Kestrel, installed into BUILD/xterm-root laid out like /boot:
#
#   bin/xterm                         the terminal emulator (static-PIE, musl)
#   share/X11/app-defaults/XTerm      its resources (menus, key bindings)
#   share/terminfo/x/xterm*           terminal descriptions (TERMINFO points here)
#   share/terminfo/l/linux            the text console's (TERM=linux), for
#                                     ncurses programs run there
#
# XTERM_SRC  an unpacked xterm release (Ubuntu's xterm_390.orig.tar.gz);
#            patches/*.patch are applied to it
# BUILD      Kestrel's build directory; BUILD/xsysroot must hold the X
#            libraries including Xt, Xmu, Xaw, SM, ICE and ncurses
#            (ports/xlibre/build-sysroot.sh)
#
# Configuration: core fonts (no FreeType), no setuid/utmp helper, no luit,
# no session management, no input methods, no Tektronix emulation. The
# shell is $SHELL, else the passwd entry's, else /bin/sh (BusyBox ash).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
mkdir -p "$2"
BUILD=$(cd "$2" && pwd)
SYSROOT=$BUILD/xsysroot
OUT=$BUILD/xterm
ROOT=$BUILD/xterm-root

[ -f "$SYSROOT/lib/libXaw7.a" ] && [ -f "$SYSROOT/lib/libncursesw.a" ] || {
    echo "xterm: $SYSROOT lacks Xaw/ncurses; run ports/xlibre/build-sysroot.sh first"; exit 1; }
"$HERE/../toolchain/install.sh" "$SYSROOT" >/dev/null

for p in "$HERE"/patches/*.patch; do
    if patch -p1 -N --dry-run -s -d "$SRC" < "$p" >/dev/null 2>&1; then
        patch -p1 -N -s -d "$SRC" < "$p"
        echo "  PATCH   $(basename "$p")"
    fi
done

# static-PIE links; partial links untouched (same wrapper as BusyBox)
CC=$BUILD/musl-pie-gcc
printf '%s\n' '#!/bin/sh' \
    'for a in "$@"; do' \
    '    case "$a" in -c|-S|-E|-r|-Wl,-r|-Wl,--relocatable|-shared) exec musl-gcc "$@";; esac' \
    'done' \
    'exec musl-gcc -static-pie "$@"' > "$CC"
chmod +x "$CC"

# HAVE_GRANTPT_PTY_ISATTY: configure cannot run its test program when
# cross-compiling; on Kestrel (as on Linux with musl) posix_openpt +
# grantpt + unlockpt + ptsname is the right interface.
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"
"$SRC/configure" --host=x86_64-linux-musl --prefix=/boot \
    CC="$CC" CFLAGS="-O2 -fPIE" \
    CPPFLAGS="-I$SYSROOT/include -I$SYSROOT/include/ncursesw -DHAVE_GRANTPT_PTY_ISATTY=1" \
    LDFLAGS="-L$SYSROOT/lib" PKG_CONFIG="$SYSROOT/bin/kestrel-pkg-config" \
    --x-includes="$SYSROOT/include" --x-libraries="$SYSROOT/lib" \
    --disable-setuid --disable-setgid --without-utempter --disable-freetype --without-xinerama \
    --disable-luit --disable-session-mgt --disable-i18n --disable-tek4014 --disable-desktop \
    --without-pcre --without-pcre2 \
    --with-app-defaults=/boot/share/X11/app-defaults \
    --with-own-terminfo=/boot/share/terminfo --enable-env-terminfo \
    --with-icondir=/boot/share/icons --with-pixmapdir=/boot/share/pixmaps >"$BUILD/xterm.log" 2>&1 \
    || { tail -20 "$BUILD/xterm.log"; exit 1; }
echo "  BUILD   xterm"
make -j"${MAKEJ:-8}" >>"$BUILD/xterm.log" 2>&1 || { grep -E "error|undefined" "$BUILD/xterm.log" | head -20; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/share/X11/app-defaults" "$ROOT/share/terminfo/x" "$ROOT/share/terminfo/l"
strip -o "$ROOT/bin/xterm" "$OUT/xterm"
cp "$SRC/XTerm.ad" "$ROOT/share/X11/app-defaults/XTerm"
TI=$(for d in /usr/share/terminfo /lib/terminfo /etc/terminfo; do [ -f "$d/x/xterm" ] && echo "$d" && break; done)
[ -n "$TI" ] || { echo "xterm: no compiled terminfo for xterm on the build host (ncurses-base)"; exit 1; }
for t in xterm xterm-256color xterm-color xterm-new; do
    [ -f "$TI/x/$t" ] && cp -L "$TI/x/$t" "$ROOT/share/terminfo/x/$t"
done
[ -f "$TI/l/linux" ] && cp -L "$TI/l/linux" "$ROOT/share/terminfo/l/linux"
touch "$ROOT/.stamp"
echo "xterm: installed into $ROOT"
