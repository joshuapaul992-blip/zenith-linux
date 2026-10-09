#!/bin/sh
# ports/xlibre/build-sysroot.sh SRC_DIR SYSROOT
#
# Builds the libraries the XLibre server needs as static musl libraries into
# SYSROOT (headers, lib*.a, pkg-config files). SRC_DIR holds the upstream
# release tarballs (*.orig.tar.* from Ubuntu's source packages work:
# `apt-get source --download-only xorgproto xtrans libxau libxdmcp xcb-proto
# libxcb libx11 libxkbfile libfontenc libxfont pixman zlib libxext libxrender
# libxfixes libxdamage libxcomposite libxpm libpng1.6`).
set -eu
SRC=$(cd "$1" && pwd)
SYSROOT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$SYSROOT/.build
mkdir -p "$WORK"

export CC="musl-gcc"
export CFLAGS="-O2 -g -fPIE"
export CPPFLAGS="-I$SYSROOT/include"
export LDFLAGS="-L$SYSROOT/lib"
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
export ACLOCAL_PATH="$SYSROOT/share/aclocal"
HOSTOPT="--host=x86_64-linux-musl --prefix=$SYSROOT --enable-static --disable-shared"

unpack() {      # unpack NAME-PREFIX -> echo directory
    t=$(ls "$SRC"/$1_*.orig.tar.* | grep -v '\.asc$' | head -1)
    d="$WORK/$1"
    rm -rf "$d"; mkdir -p "$d"
    tar -xf "$t" -C "$d" --strip-components=1
    # the distribution's fixes: a .diff.gz, or debian/patches from a .debian.tar.*
    diff=$(ls "$SRC"/$1_*.diff.gz 2>/dev/null | head -1 || true)
    deb=$(ls "$SRC"/$1_*.debian.tar.* 2>/dev/null | head -1 || true)
    if [ -n "$diff" ]; then zcat "$diff" | (cd "$d" && patch -p1 -s >/dev/null); fi
    if [ -n "$deb" ]; then tar -xf "$deb" -C "$d"; fi
    if [ -f "$d/debian/patches/series" ]; then
        grep -v '^#' "$d/debian/patches/series" | while read -r p _; do
            [ -n "$p" ] && (cd "$d" && patch -p1 -s < "debian/patches/$p" >/dev/null) || true
        done
    fi
    echo "$d"
}

autotools() {   # NAME [configure args...]
    n=$1; shift
    [ -f "$WORK/$n.done" ] && return 0
    echo "=== $n"
    d=$(unpack "$n")
    (cd "$d" && ./configure $HOSTOPT "$@" >"$WORK/$n.log" 2>&1 && make -j${MAKEJ:-8} >>"$WORK/$n.log" 2>&1 && make install >>"$WORK/$n.log" 2>&1) \
        || { echo "$n failed, see $WORK/$n.log"; tail -20 "$WORK/$n.log"; exit 1; }
    touch "$WORK/$n.done"
}

mesonbuild() {  # NAME [meson args...]
    n=$1; shift
    [ -f "$WORK/$n.done" ] && return 0
    echo "=== $n"
    d=$(unpack "$n")
    (cd "$d" && meson setup _b --prefix="$SYSROOT" --libdir=lib --default-library=static --buildtype=release "$@" \
        >"$WORK/$n.log" 2>&1 && ninja -C _b install >>"$WORK/$n.log" 2>&1) \
        || { echo "$n failed, see $WORK/$n.log"; tail -20 "$WORK/$n.log"; exit 1; }
    touch "$WORK/$n.done"
}

# zlib: its own configure (no --host)
if [ ! -f "$WORK/zlib.done" ]; then
    echo "=== zlib"
    d=$(unpack zlib)
    (cd "$d" && CC=musl-gcc ./configure --prefix="$SYSROOT" --static >"$WORK/zlib.log" 2>&1 && make -j8 install >>"$WORK/zlib.log" 2>&1) \
        || { echo "zlib failed"; tail -20 "$WORK/zlib.log"; exit 1; }
    touch "$WORK/zlib.done"
fi

mesonbuild xorgproto -Dlegacy=false
autotools  xtrans
autotools  libxau
autotools  libxdmcp
autotools  xcb-proto
MAKEJ=1 autotools  libxcb --disable-devel-docs --without-doxygen
autotools  libx11 --disable-specs --without-xmlto --without-fop --disable-loadable-i18n --disable-xf86bigfont
autotools  libxkbfile
autotools  libfontenc
autotools  libxfont --disable-devel-docs --disable-freetype --disable-bzip2
mesonbuild pixman -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled -Dtests=disabled

# client libraries for window managers and toolkits (IceWM)
autotools  libxext --disable-specs --without-xmlto --without-fop
autotools  libxrender
autotools  libxfixes
autotools  libxdamage
autotools  libxcomposite --disable-doc --without-xmlto
autotools  libxpm --disable-open-zfile --disable-stat-zfile
autotools  libpng1.6 --disable-tools

# libsha1 (ours)
if [ ! -f "$WORK/libsha1.done" ]; then
    echo "=== libsha1"
    musl-gcc $CFLAGS -c "$HERE/libsha1.c" -I"$HERE" -o "$WORK/libsha1.o"
    ar rcs "$SYSROOT/lib/libsha1.a" "$WORK/libsha1.o"
    cp "$HERE/libsha1.h" "$SYSROOT/include/"
    cat > "$SYSROOT/lib/pkgconfig/libsha1.pc" <<PC
prefix=$SYSROOT
Name: libsha1
Description: SHA1 for the X server (Kestrel)
Version: 0.3
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -lsha1
PC
    touch "$WORK/libsha1.done"
fi
# Everything is static: put the private dependencies on the Libs lines so
# a plain `pkg-config --libs` (what meson asks for) links completely.
sed -i 's|^Libs: \(.*-lXfont2\)$|Libs: \1 -lfontenc -lz|' "$SYSROOT/lib/pkgconfig/xfont2.pc"
sed -i 's|^Libs: \(.*-lfontenc\)$|Libs: \1 -lz|' "$SYSROOT/lib/pkgconfig/fontenc.pc"
echo "sysroot ready: $SYSROOT"
