#!/bin/sh
# Usage: apply.sh <AROS checkout>
#
# Applies the Pascal patch series to a git checkout of
# https://github.com/deadw00d/AROS (written against master 5b5fd4cf).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "$1" ] || [ ! -d "$1/workbench/hidds/nouveau" ]; then
    echo "usage: $0 <AROS checkout>" >&2
    exit 2
fi

cd "$1"
git am --3way "$HERE"/patches/*.patch
