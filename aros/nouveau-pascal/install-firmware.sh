#!/bin/sh
# Usage: install-firmware.sh <Linux firmware dir> <AROS Devs/Firmware dir>
#
# Copies NVIDIA's signed Pascal firmware (GP102, GP104, GP106, GP107, GP108)
# from a Linux firmware tree, e.g. /lib/firmware, into the Devs/Firmware
# directory of an AROS installation, where nouveau.hidd looks for
# DEVS:Firmware/nvidia/<chip>/... .
#
# Linux distributions often ship these files compressed (.xz, .zst) and as
# symlinks shared between chips. AROS needs plain files, so links are
# followed and compressed files are unpacked.
set -e

SRC=$1
DST=$2
if [ -z "$SRC" ] || [ -z "$DST" ] || [ ! -d "$SRC/nvidia" ]; then
    echo "usage: $0 <Linux firmware dir> <AROS Devs/Firmware dir>" >&2
    echo "example: $0 /lib/firmware /mnt/aros/Devs/Firmware" >&2
    exit 2
fi

copied=0
for chip in gp102 gp104 gp106 gp107 gp108; do
    if [ ! -d "$SRC/nvidia/$chip" ]; then
        echo "skip  nvidia/$chip (not in $SRC)"
        continue
    fi

    for f in $(cd "$SRC/nvidia/$chip" && find . \( -type f -o -type l \) | sort); do
        in="$SRC/nvidia/$chip/${f#./}"
        if [ ! -e "$in" ]; then
            echo "skip  nvidia/$chip/${f#./} (broken link)" >&2
            continue
        fi

        case "$in" in
            *.xz)  out="$DST/nvidia/$chip/${f#./}"; out="${out%.xz}" ;;
            *.zst) out="$DST/nvidia/$chip/${f#./}"; out="${out%.zst}" ;;
            *)     out="$DST/nvidia/$chip/${f#./}" ;;
        esac
        mkdir -p "$(dirname "$out")"

        case "$in" in
            *.xz)  xz -dc "$in" > "$out" ;;
            *.zst) zstd -qdc "$in" > "$out" ;;
            *)     cp -L "$in" "$out" ;;
        esac
        echo "copy  ${out#$DST/}"
        copied=$((copied + 1))
    done
done

echo "$copied file(s) copied to $DST"
