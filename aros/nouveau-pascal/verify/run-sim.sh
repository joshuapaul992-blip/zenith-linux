#!/bin/sh
# Usage: run-sim.sh <AROS checkout> [--original]
#
# Builds the nouveau falcon message queue sources from the given checkout,
# configured for AROS, against sim/harness.c (a fake SEC2 falcon running
# the 0x0148cdec RTOS protocol) and runs it.
#
# --original runs against an unpatched checkout. It is expected to stop in
# NOT_IMPLEMENTED_STOP, so it is killed after 15 seconds.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
NV="$1/workbench/hidds/nouveau"
if [ -z "$1" ] || [ ! -d "$NV/drm/nouveau" ]; then
    echo "usage: $0 <AROS checkout> [--original]" >&2
    exit 2
fi

EXTRA=""
LIMIT=300
if [ "$2" = "--original" ]; then
    EXTRA="-DSIM_ORIGINAL_HEADERS"
    LIMIT=15
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="$EXTRA -std=gnu99 -D_POSIX_C_SOURCE=200809L -D__AROS__ -O1 -g
  -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
  -Werror=int-conversion -include $HERE/shim/prelude.h
  -I$HERE/sim/include -I$HERE/shim -I$NV/include -I$NV/drm/nouveau
  -I$NV/drm/nouveau/include -I$NV/drm/nouveau/include/nvkm
  -I$NV/drm/nouveau/nvkm -I$NV/drm/nouveau/nvkm/falcon"

for f in msgqueue msgqueue_0148cdec msgqueue_0137c63d; do
    gcc $CFLAGS -c "$NV/drm/nouveau/nvkm/falcon/$f.c" -o "$OUT/$f.o" 2>/dev/null
done
gcc $CFLAGS -c "$HERE/sim/harness.c" -o "$OUT/harness.o" 2>/dev/null
gcc -pthread "$OUT"/*.o -o "$OUT/msgqueue-sim"

timeout "$LIMIT" "$OUT/msgqueue-sim"
