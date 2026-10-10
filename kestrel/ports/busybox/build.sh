#!/bin/sh
# ports/busybox/build.sh BUSYBOX_SRC BUILD
#
# BusyBox for Kestrel: a POSIX shell (ash) and the usual command-line tools
# in one static-PIE musl binary, installed into BUILD/busybox-root/bin with
# every applet as a hard link to bin/busybox. The boot volume keeps the
# links (tarfs hard links), and /boot/bin is bound on /bin, so /bin/sh,
# /bin/ls, ... exist where programs expect them.
#
# BUSYBOX_SRC  an unpacked BusyBox release (Ubuntu's busybox_1.36.1.orig.tar.bz2)
# BUILD        Kestrel's build directory
#
# Host needs: musl-gcc, gcc, make, and the Linux UAPI headers (linux-libc-dev)
# that some applets include.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$1" && pwd)
mkdir -p "$2"
BUILD=$(cd "$2" && pwd)
OUT=$BUILD/busybox
ROOT=$BUILD/busybox-root

# musl's headers plus the kernel's UAPI headers (linux/, asm/, asm-generic/)
KH=$BUILD/kheaders
mkdir -p "$KH"
ARCH=$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)
for d in linux asm-generic mtd; do [ -e "$KH/$d" ] || ln -s "/usr/include/$d" "$KH/$d"; done
[ -e "$KH/asm" ] || ln -s "/usr/include/$ARCH/asm" "$KH/asm"

# musl-gcc that links final executables as static PIE, but not the partial
# (-r) links busybox makes on the way
CC=$BUILD/musl-pie-gcc
printf '%s\n' '#!/bin/sh' \
    'for a in "$@"; do' \
    '    case "$a" in -c|-S|-E|-r|-Wl,-r|-Wl,--relocatable|-shared) exec musl-gcc "$@";; esac' \
    'done' \
    'exec musl-gcc -static-pie "$@"' > "$CC"
chmod +x "$CC"

rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$SRC/." "$OUT/"
cd "$OUT"
make -s CC="$CC" HOSTCC=gcc defconfig >/dev/null

# Configuration: start from defconfig, then
set_opt() { sed -i "/^# $1 is not set\$/d; /^$1=/d" .config; echo "$1=$2" >> .config; }
set_off() { sed -i "/^$1=/d; /^# $1 is not set\$/d" .config; echo "# $1 is not set" >> .config; }
set_opt CONFIG_EXTRA_CFLAGS "\"-fPIE -isystem $KH\""
set_off CONFIG_STATIC                       # -static-pie instead (Kestrel loads PIE images)
set_off CONFIG_PIE
set_opt CONFIG_INSTALL_APPLET_HARDLINKS y
set_off CONFIG_INSTALL_APPLET_SYMLINKS
set_opt CONFIG_INSTALL_NO_USR y
set_opt CONFIG_BUSYBOX_EXEC_PATH '"/bin/busybox"'   # no /proc/self/exe
# Applets that do not build against musl + UAPI headers, or need kernel
# features Kestrel lacks entirely (networking, modules, mounts, consoles):
for o in $(cat "$HERE/disabled-applets"); do set_off "$o"; done
yes "" | make -s CC="$CC" HOSTCC=gcc oldconfig >/dev/null

echo "  BUILD   busybox"
make -s -j"${MAKEJ:-8}" CC="$CC" HOSTCC=gcc >"$BUILD/busybox.log" 2>&1 || {
    grep -E "error|Error" "$BUILD/busybox.log" | head -20; exit 1; }

rm -rf "$ROOT"
make -s CC="$CC" HOSTCC=gcc CONFIG_PREFIX="$ROOT" install >/dev/null
# one directory: everything into bin/ (hard links to bin/busybox)
if [ -d "$ROOT/sbin" ]; then
    for f in "$ROOT"/sbin/*; do ln -f "$ROOT/bin/busybox" "$ROOT/bin/$(basename "$f")"; done
    rm -rf "$ROOT/sbin"
fi
rm -f "$ROOT/linuxrc"
strip "$ROOT/bin/busybox"
for f in "$ROOT"/bin/*; do [ "$f" = "$ROOT/bin/busybox" ] || ln -f "$ROOT/bin/busybox" "$f"; done
touch "$ROOT/.stamp"
echo "busybox: $(ls "$ROOT/bin" | wc -l) applets in $ROOT/bin ($(stat -c %s "$ROOT/bin/busybox") bytes)"
