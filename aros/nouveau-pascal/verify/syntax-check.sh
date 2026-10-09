#!/bin/sh
# Usage: syntax-check.sh <AROS checkout>
#
# Host-compiler syntax/type check of the nouveau DRM sources touched by the
# patch series, compiled with -D__AROS__ against the driver's own headers.
# The headers in shim/ stand in only for AROS system headers that an AROS
# build generates. This is not a substitute for building with the AROS
# cross toolchain.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
NV="$1/workbench/hidds/nouveau"
if [ -z "$1" ] || [ ! -d "$NV/drm/nouveau" ]; then
    echo "usage: $0 <AROS checkout>" >&2
    exit 2
fi

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
status=0

for f in nvkm/falcon/falcon_root.c nvkm/falcon/msgqueue_0148cdec.c \
         nvkm/falcon/msgqueue_0137c63d.c nouveau_dp.c; do
    if gcc -fsyntax-only -std=gnu99 -D_POSIX_C_SOURCE=200809L -D__AROS__ \
        -Werror=implicit-function-declaration \
        -Werror=incompatible-pointer-types -Werror=int-conversion \
        -include "$HERE/shim/prelude.h" -I"$HERE/shim" \
        -I"$NV/include" -I"$NV/drm/nouveau" -I"$NV/drm/nouveau/include" \
        -I"$NV/drm/nouveau/include/nvkm" -I"$NV/drm/nouveau/nvkm" \
        "$NV/drm/nouveau/$f" 2>"$LOG"; then
        echo "ok    $f"
    else
        echo "FAIL  $f"
        grep "error" "$LOG" | sed "s|$NV/||"
        status=1
    fi
done

exit $status
