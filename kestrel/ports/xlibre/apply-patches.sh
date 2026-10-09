#!/bin/sh
# ports/xlibre/apply-patches.sh XLIBRE_SRC
#
# Turns an XLibre source tree into one that builds Xkestrel:
#   - hw/kestrel (the Kestrel DDX) and a `kestrel` meson option
#   - patches/*.patch (Kestrel-specific behaviour, guarded by KESTREL_OS)
# Idempotent: running it twice changes nothing the second time.
set -eu
SRC=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$SRC/hw/kestrel"
cp "$HERE/hw-kestrel/kestrel.c" "$HERE/hw-kestrel/meson.build" "$SRC/hw/kestrel/"

grep -q "option('kestrel'" "$SRC/meson_options.txt" || cat >> "$SRC/meson_options.txt" <<'OPT'
option('kestrel', type: 'boolean', value: false,
       description: 'Enable Xkestrel, the X server for the Kestrel operating system')
OPT

grep -q "build_kestrel" "$SRC/meson.build" || \
    sed -i "s/^build_xvfb = get_option('xvfb')$/build_xvfb = get_option('xvfb')\nbuild_kestrel = get_option('kestrel')/" "$SRC/meson.build"
grep -q "subdir('kestrel')" "$SRC/hw/meson.build" || \
    printf "\nif build_kestrel\n    subdir('kestrel')\nendif\n" >> "$SRC/hw/meson.build"

for p in "$HERE"/patches/*.patch; do
    if patch -p1 -N --dry-run -s -d "$SRC" < "$p" >/dev/null 2>&1; then
        patch -p1 -N -s -d "$SRC" < "$p"
        echo "applied $(basename "$p")"
    fi
done
